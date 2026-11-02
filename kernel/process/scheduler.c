#include "scheduler.h"
#include "syscall.h"
#include "percpu.h"
#include "gdt.h"
#include "log.h"

static thread_t* g_thread_list = NULL;
static thread_t* g_current_thread = NULL;
static thread_t* g_idle_thread = NULL;
static thread_t* g_last_thread = NULL;
static uint64_t g_tick_count = 0;
static bool g_scheduler_ready = false;
static bool g_idle_reported = false;

void scheduler_init(void) {
    g_thread_list = NULL;
    g_current_thread = NULL;
    g_idle_thread = NULL;
    g_last_thread = NULL;
    g_tick_count = 0;
    g_scheduler_ready = false;
    g_idle_reported = false;
}

void scheduler_add_thread(thread_t* thread) {
    if (!thread) return;

    if (!g_thread_list) {
        g_thread_list = thread;
        g_last_thread = thread;
    } else {
        g_last_thread->next = thread;
        g_last_thread = thread;
    }

    if (thread->is_idle && !g_idle_thread) {
        g_idle_thread = thread;
    }
}

thread_t* scheduler_current_thread(void) {
    return g_current_thread;
}

uint64_t scheduler_get_ticks(void) {
    return g_tick_count;
}

static bool has_runnable_user(void) {
    for (thread_t* t = g_thread_list; t; t = t->next) {
        if (!t->is_idle && t->state == THREAD_READY) return true;
    }
    return false;
}

bool scheduler_has_runnable_user(void) {
    return has_runnable_user();
}

static thread_t* choose_next(thread_t* after, bool skip_after) {
    thread_t* start = (after && after->next) ? after->next : g_thread_list;

    for (thread_t* t = start; t; t = t->next) {
        if (t != after && !t->is_idle && t->state == THREAD_READY) {
            return t;
        }
    }
    for (thread_t* t = g_thread_list; t && t != start; t = t->next) {
        if (t != after && !t->is_idle && t->state == THREAD_READY) {
            return t;
        }
    }

    if (!skip_after && after && after->state == THREAD_READY) {
        return after;
    }

    return (g_idle_thread && g_idle_thread->state == THREAD_READY)
               ? g_idle_thread
               : NULL;
}

static void switch_to(thread_t* next, bool current_gs_is_kernel) {
    if (!next) return;
    (void)current_gs_is_kernel;

    thread_t* previous = g_current_thread;
    if (previous != next) {
        if (previous) {
            fpu_save(previous->fpu_state);
        }

        process_set_current(next->process);
        per_cpu_set_current(next);
        per_cpu_set_kernel_stack(next->kernel_stack_top);
        per_cpu_set_gs_domain(next->is_user);
        tss_set_rsp0(next->kernel_stack_top);
        vmm_switch_pml4(next->process->pml4);

        fpu_restore(next->fpu_state);
        per_cpu_block.switch_gs = 0;
        g_current_thread = next;
    } else {
        per_cpu_set_gs_domain(next->is_user);
        per_cpu_block.switch_gs = 0;
    }

    next->state = THREAD_RUNNING;
    if (!next->is_idle) {
        next->process->state = PROCESS_RUNNING;
        if (next->quantum == 0) next->quantum = SCHEDULER_QUANTUM_TICKS;
    }
}

void scheduler_start(void) {
    if (!g_idle_thread) {
        log_printf("[SCHED FATAL] Idle thread is missing.\n");
        for (;;) __asm__ volatile ("pause");
    }

    g_current_thread = g_idle_thread;
    g_idle_thread->state = THREAD_RUNNING;
    process_set_current(g_idle_thread->process);
    per_cpu_set_current(g_idle_thread);
    per_cpu_set_kernel_stack(g_idle_thread->kernel_stack_top);
    tss_set_rsp0(g_idle_thread->kernel_stack_top);
    per_cpu_clear_reschedule();
    g_scheduler_ready = true;

    thread_t* first = choose_next(g_idle_thread, true);
    if (!first) first = g_idle_thread;

    log_printf("[SCHED] Starting preemptive round-robin scheduler (quantum=%u ticks).\n",
               SCHEDULER_QUANTUM_TICKS);
    switch_to(first, true);
    arch_enter_thread(first->resume_frame);
}

void scheduler_idle_wait(void) {
    __asm__ volatile ("cli" : : : "memory");
    if (has_runnable_user()) {
        __asm__ volatile ("sti" : : : "memory");
        return;
    }

    if (!g_idle_reported) {
        g_idle_reported = true;
        log_printf("[SCHED] Idle thread sleeping with atomic STI;HLT window.\n");
    }

    /*
     * STI inhibits interrupts until after the following instruction. This
     * closes the check-then-sleep race on a uniprocessor and keeps the guest
     * CPU active enough for VMware/QEMU to observe a live OS.
     */
    __asm__ volatile ("sti; hlt" : : : "memory");
}

void scheduler_idle_entry(void) {
    for (;;) {
        scheduler_idle_wait();
    }
}

static void wake_sleepers(void) {
    for (thread_t* t = g_thread_list; t; t = t->next) {
        if (t->state == THREAD_BLOCKED && t->wake_tick != 0 &&
            g_tick_count >= t->wake_tick) {
            t->wake_tick = 0;
            t->state = THREAD_READY;
            if (t->process) t->process->state = PROCESS_READY;
            log_printf("[SCHED] Woke TID %llu from timed sleep.\n",
                       (unsigned long long)t->tid);
        }
    }
}

trap_frame_t* scheduler_handle_irq(trap_frame_t* frame) {
    if (!g_scheduler_ready || !g_current_thread) return frame;

    g_tick_count++;
    wake_sleepers();

    thread_t* current = g_current_thread;
    if (current->state != THREAD_RUNNING) {
        thread_t* next = choose_next(current, true);
        if (!next) return frame;
        switch_to(next, !current->is_user);
        return next->resume_frame;
    }

    if (current->is_idle) {
        if (!has_runnable_user()) {
            per_cpu_block.switch_gs = 0;
            return frame;
        }

        current->resume_frame = frame;
        thread_t* next = choose_next(current, true);
        if (!next || next == current) return frame;
        switch_to(next, true);
        return next->resume_frame;
    }

    if (current->is_user) {
        current->resume_frame = frame;
        if (current->wait_queue_id != 0) {
            /* A blocked queue waiter should not be running. */
            current->wait_queue_id = 0;
        }
    }

    if (current->quantum > 0) current->quantum--;
    if (current->quantum > 0) {
        per_cpu_block.switch_gs = 0;
        return frame;
    }

    current->state = THREAD_READY;
    if (current->process) current->process->state = PROCESS_READY;
    current->quantum = SCHEDULER_QUANTUM_TICKS;

    thread_t* next = choose_next(current, false);
    if (!next) next = current;
    if (next == current) {
        current->state = THREAD_RUNNING;
        if (current->process) current->process->state = PROCESS_RUNNING;
        per_cpu_block.switch_gs = 0;
        return frame;
    }

    switch_to(next, !current->is_user);
    return next->resume_frame;
}

trap_frame_t* scheduler_reschedule_from_syscall(trap_frame_t* frame) {
    thread_t* current = g_current_thread;
    if (!current) return frame;

    if (current->state != THREAD_ZOMBIE) {
        current->resume_frame = frame;
    }
    if (current->state == THREAD_RUNNING) {
        current->state = THREAD_READY;
        if (current->process) current->process->state = PROCESS_READY;
    }

    per_cpu_clear_reschedule();
    thread_t* next = choose_next(current, true);
    if (!next) next = current;

    switch_to(next, true);
    return next->resume_frame;
}

void scheduler_request_reschedule(void) {
    per_cpu_request_reschedule();
}

void scheduler_mark_current_ready(void) {
    if (g_current_thread && g_current_thread->state == THREAD_RUNNING) {
        g_current_thread->state = THREAD_READY;
        if (g_current_thread->process) {
            g_current_thread->process->state = PROCESS_READY;
        }
    }
}

void scheduler_block_current(uint64_t wake_tick) {
    if (!g_current_thread) return;
    g_current_thread->wake_tick = wake_tick;
    g_current_thread->state = THREAD_BLOCKED;
    if (g_current_thread->process) {
        g_current_thread->process->state = PROCESS_BLOCKED;
    }
    per_cpu_request_reschedule();
}

void scheduler_wake_thread(thread_t* thread) {
    if (!thread || thread->state != THREAD_BLOCKED) return;
    thread->wake_tick = 0;
    thread->wait_queue_id = 0;
    thread->state = THREAD_READY;
    if (thread->process) thread->process->state = PROCESS_READY;
}

thread_t* scheduler_find_ipc_waiter(uint64_t queue_id) {
    for (thread_t* t = g_thread_list; t; t = t->next) {
        if (t->state == THREAD_BLOCKED && t->wait_queue_id == queue_id) {
            return t;
        }
    }
    return NULL;
}
