#include "scheduler.h"
#include "syscall.h"
#include "percpu.h"
#include "gdt.h"
#include "pmm.h"
#include "heap.h"
#include "log.h"
#include "usb_core.h"
#include "ui_compat.h"
#include "power.h"
#include "pxe.h"

extern void audio_poll(void);

static thread_t* g_thread_list = NULL;
static thread_t* g_current_thread = NULL;
static thread_t* g_idle_thread = NULL;
static thread_t* g_compositor_thread = NULL;
static thread_t* g_last_thread = NULL;
static uint64_t g_tick_count = 0;
static uint64_t g_boot_switch_logs = 0;
static bool g_scheduler_ready = false;
static bool g_idle_reported = false;
extern bool g_acpi_shutdown_test_requested;

void scheduler_init(void) {
    g_thread_list = NULL;
    g_current_thread = NULL;
    g_idle_thread = NULL;
    g_compositor_thread = NULL;
    g_last_thread = NULL;
    g_tick_count = 0;
    g_boot_switch_logs = 0;
    g_scheduler_ready = false;
    g_idle_reported = false;
}

void scheduler_add_thread(thread_t* thread) {
    if (!thread) return;

    uint64_t rflags = 0;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags) : : "memory");

    thread->next = NULL;
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
    if (thread->is_compositor &&
        (!g_compositor_thread || g_compositor_thread->state == THREAD_ZOMBIE)) {
        g_compositor_thread = thread;
    }

    if (rflags & 0x200ULL) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

thread_t* scheduler_current_thread(void) {
    return g_current_thread;
}

uint64_t scheduler_get_ticks(void) {
    return g_tick_count;
}

void scheduler_yield_kernel(void) {
    __asm__ volatile ("int $0x81" : : : "memory");
}

static bool has_runnable_non_idle(void) {
    for (thread_t* t = g_thread_list; t; t = t->next) {
        if (!t->is_idle && t->state == THREAD_READY) return true;
    }
    return false;
}

bool scheduler_has_runnable_user(void) {
    return has_runnable_non_idle();
}

static thread_t* choose_next(thread_t* after, bool skip_after) {
    /* High-priority desktop compositor always preempts normal tasks */
    if (g_compositor_thread) {
        if (g_compositor_thread != after &&
            g_compositor_thread->state == THREAD_READY) {
            return g_compositor_thread;
        }
        if (g_compositor_thread == after && !skip_after &&
            g_compositor_thread->state == THREAD_READY) {
            return g_compositor_thread;
        }
    }

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

    if (!skip_after && after && !after->is_idle && after->state == THREAD_READY) {
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
        if (previous && previous->is_idle &&
            previous->state == THREAD_RUNNING) {
            previous->state = THREAD_READY;
            if (previous->process) previous->process->state = PROCESS_READY;
        }
        if (g_boot_switch_logs < 6) {
            g_boot_switch_logs++;
            log_printf("[SCHED] switch pid %u -> %u, resume RIP=0x%016llx\n",
                       previous && previous->process ? previous->process->pid : 0,
                       next->process ? next->process->pid : 0,
                       next->resume_frame ? (unsigned long long)next->resume_frame->rip : 0);
        }
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
        if (next->quantum == 0) {
            next->quantum = next->is_compositor ? 2 : SCHEDULER_QUANTUM_TICKS;
        }
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

    log_printf("[SCHED] Starting preemptive scheduler (quantum=%u ticks, compositor=PID %u).\n",
               SCHEDULER_QUANTUM_TICKS,
               g_compositor_thread && g_compositor_thread->process
                   ? g_compositor_thread->process->pid : 0);
    switch_to(first, true);
    arch_enter_thread(first->resume_frame);
}

void scheduler_idle_wait(void) {
    __asm__ volatile ("cli" : : : "memory");
    if (has_runnable_non_idle()) {
        __asm__ volatile ("sti" : : : "memory");
        scheduler_yield_kernel();
        return;
    }

    if (!g_idle_reported) {
        g_idle_reported = true;
        log_printf("[SCHED] Idle thread sleeping with atomic STI;HLT window.\n");
    }

    __asm__ volatile ("sti; hlt" : : : "memory");
}

void scheduler_idle_entry(void) {
    for (;;) {
        scheduler_idle_wait();
    }
}

void scheduler_reap_zombies(void) {
    process_t* reap_head = NULL;

    __asm__ volatile ("cli" : : : "memory");
    thread_t** tp = &g_thread_list;
    thread_t* prev = NULL;
    while (*tp) {
        thread_t* t = *tp;
        process_t* p = t->process;
        bool can_reap = false;
        if (t != g_current_thread && !t->is_idle && t != g_compositor_thread &&
            t->state == THREAD_ZOMBIE && p && p->state == PROCESS_ZOMBIE) {
            if (!p->parent || p->waited ||
                p->parent->state == PROCESS_ZOMBIE ||
                p->parent->is_system_daemon ||
                g_tick_count >= p->zombie_reap_tick) {
                can_reap = true;
            }
        }

        if (can_reap) {
            *tp = t->next;
            if (g_last_thread == t) {
                g_last_thread = prev;
            }
            t->next = NULL;
            process_unlink(p);
            p->next = reap_head;
            reap_head = p;
        } else {
            prev = t;
            tp = &t->next;
        }
    }
    __asm__ volatile ("sti" : : : "memory");

    while (reap_head) {
        process_t* p = reap_head;
        reap_head = p->next;
        p->next = NULL;

        process_cleanup_runtime_resources(p);

        thread_t* t = p->main_thread;
        if (t) {
            if (t->kernel_stack_phys) {
                pmm_free_pages(t->kernel_stack_phys, t->kernel_stack_pages);
            }
            kfree(t);
            p->main_thread = NULL;
        }

        if (p->r0_slot >= 0) {
            pxe_release_r0_image(p);
        }

        if (p->exec_ring == 3 && p->pml4 && p->pml4 != vmm_get_kernel_pml4()) {
            vmm_destroy_address_space(p->pml4);
            p->pml4 = NULL;
        }

        user_region_t* reg = p->regions;
        while (reg) {
            user_region_t* next_reg = reg->next;
            kfree(reg);
            reg = next_reg;
        }
        p->regions = NULL;

        kfree(p);
    }
}

void scheduler_compositor_entry(void) {
    for (;;) {
        usb_poll();
        audio_poll();
        ui_desktop_poll();
        audio_poll();
        scheduler_reap_zombies();

        if (g_acpi_shutdown_test_requested && g_tick_count >= 200) {
            g_acpi_shutdown_test_requested = false;
            log_printf("[ACPI TEST] Scheduler alive; starting direct S5 shutdown test\n");
            sys_shutdown();
        }

        scheduler_block_current(scheduler_get_ticks() + 1);
        scheduler_yield_kernel();
    }
}

static void wake_sleepers(void) {
    for (thread_t* t = g_thread_list; t; t = t->next) {
        if (t->state == THREAD_BLOCKED && t->wake_tick != 0 &&
            g_tick_count >= t->wake_tick) {
            t->wake_tick = 0;
            t->wait_win_id = -1;
            t->state = THREAD_READY;
            if (t->process) t->process->state = PROCESS_READY;
        }
    }
}

trap_frame_t* scheduler_handle_irq(trap_frame_t* frame) {
    if (!g_scheduler_ready || !g_current_thread) return frame;

    g_tick_count++;
    wake_sleepers();

    thread_t* current = g_current_thread;
    if (current->process && !current->is_idle && current->state == THREAD_RUNNING) {
        current->process->cpu_ticks_total++;
    }

    if (current->state != THREAD_RUNNING) {
        if (current->state != THREAD_ZOMBIE) {
            current->resume_frame = frame;
        }
        thread_t* next = choose_next(current, true);
        if (!next) return frame;
        switch_to(next, !current->is_user);
        return next->resume_frame;
    }

    if (current->is_idle) {
        if (!has_runnable_non_idle()) {
            per_cpu_block.switch_gs = 0;
            return frame;
        }

        current->resume_frame = frame;
        thread_t* next = choose_next(current, true);
        if (!next || next == current) return frame;
        switch_to(next, true);
        return next->resume_frame;
    }

    /* Always save current resume_frame for both Ring 3 and Ring 0 threads! */
    current->resume_frame = frame;
    if (current->wait_queue_id != 0) {
        current->wait_queue_id = 0;
    }

    /* Immediate preemption when high-priority compositor wakes up */
    if (!current->is_compositor && g_compositor_thread &&
        g_compositor_thread->state == THREAD_READY) {
        current->state = THREAD_READY;
        if (current->process) current->process->state = PROCESS_READY;
        switch_to(g_compositor_thread, !current->is_user);
        return g_compositor_thread->resume_frame;
    }

    if (current->quantum > 0) current->quantum--;
    if (current->quantum > 0) {
        per_cpu_block.switch_gs = 0;
        return frame;
    }

    current->state = THREAD_READY;
    if (current->process) current->process->state = PROCESS_READY;
    current->quantum = current->is_compositor ? 2 : SCHEDULER_QUANTUM_TICKS;

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
    if (!next) {
        if (current->state != THREAD_ZOMBIE && current->state != THREAD_BLOCKED) {
            next = current;
        } else {
            next = g_idle_thread;
        }
    }
    if (!next) return frame;

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

void scheduler_block_on_window(int32_t win_id, uint64_t timeout_ticks) {
    if (!g_current_thread) return;
    g_current_thread->wait_win_id = win_id;
    g_current_thread->wake_tick = timeout_ticks ? (g_tick_count + timeout_ticks) : 0;
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
    thread->wait_win_id = -1;
    thread->state = THREAD_READY;
    if (thread->process) thread->process->state = PROCESS_READY;
}

void scheduler_wake_win_waiters(int32_t win_id) {
    for (thread_t* t = g_thread_list; t; t = t->next) {
        if (t->state == THREAD_BLOCKED &&
            (t->wait_win_id == win_id || t->wait_win_id == 0)) {
            t->wake_tick = 0;
            t->wait_win_id = -1;
            t->state = THREAD_READY;
            if (t->process) t->process->state = PROCESS_READY;
        }
    }
}

thread_t* scheduler_find_ipc_waiter(uint64_t queue_id) {
    for (thread_t* t = g_thread_list; t; t = t->next) {
        if (t->state == THREAD_BLOCKED && t->wait_queue_id == queue_id) {
            return t;
        }
    }
    return NULL;
}
