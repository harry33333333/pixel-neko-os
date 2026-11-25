#include "scheduler.h"
#include "syscall.h"
#include "percpu.h"
#include "smp.h"
#include "apic.h"
#include "cpu.h"
#include "gdt.h"
#include "pmm.h"
#include "heap.h"
#include "spinlock.h"
#include "neko_abi.h"
#include "log.h"
#include "usb_core.h"
#include "ui_compat.h"
#include "power.h"
#include "pxe.h"
#include "gfx_accel.h"

extern void audio_poll(void);

static spinlock_t g_sched_lock = SPINLOCK_INIT;
static thread_t* g_thread_list = NULL;
static thread_t* g_last_thread = NULL;
static thread_t* g_compositor_thread = NULL;
static volatile uint64_t g_tick_count_100hz = 0;
static volatile uint64_t g_tick_count_1khz = 0;
static uint64_t g_boot_switch_logs = 0;
static volatile bool g_scheduler_ready = false;
static volatile bool g_idle_reported = false;
extern bool g_acpi_shutdown_test_requested;

void scheduler_init(void) {
    spin_lock_init(&g_sched_lock);
    g_thread_list = NULL;
    g_last_thread = NULL;
    g_compositor_thread = NULL;
    g_tick_count_100hz = 0;
    g_tick_count_1khz = 0;
    g_boot_switch_logs = 0;
    g_scheduler_ready = false;
    g_idle_reported = false;
}

bool scheduler_is_ready(void) {
    return __atomic_load_n(&g_scheduler_ready, __ATOMIC_ACQUIRE);
}

static void send_resched_ipi_if_idle_locked(int32_t target_pin) {
    uint32_t total = smp_get_cpu_count();
    if (total <= 1 || !g_scheduler_ready) return;

    uint32_t my_id = this_cpu_id();
    for (uint32_t i = 0; i < total && i < SMP_MAX_CPUS; i++) {
        if (i == my_id || !g_per_cpus[i].online) continue;
        if (target_pin >= 0 && (int32_t)i != target_pin) continue;
        thread_t* cur = (thread_t*)g_per_cpus[i].current_thread;
        if (!cur || cur->is_idle) {
            lapic_send_ipi(g_per_cpus[i].lapic_id, VEC_IPI_RESCHED);
            return;
        }
    }
}

void scheduler_add_thread(thread_t* thread) {
    if (!thread) return;

    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    thread->next = NULL;
    thread->running_cpu = -1;
    if (!g_thread_list) {
        g_thread_list = thread;
        g_last_thread = thread;
    } else {
        g_last_thread->next = thread;
        g_last_thread = thread;
    }

    if (thread->is_idle) {
        int32_t pin = (thread->pinned_cpu >= 0) ? thread->pinned_cpu : 0;
        if (pin < (int32_t)SMP_MAX_CPUS) {
            g_per_cpus[pin].idle_thread = thread;
        }
    }
    if (thread->is_compositor &&
        (!g_compositor_thread || g_compositor_thread->state == THREAD_ZOMBIE)) {
        g_compositor_thread = thread;
    }

    if (!thread->is_idle && thread->state == THREAD_READY) {
        send_resched_ipi_if_idle_locked(thread->pinned_cpu);
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
}

thread_t* scheduler_current_thread(void) {
    return (thread_t*)this_cpu()->current_thread;
}

uint64_t scheduler_get_ticks(void) {
    return g_tick_count_100hz;
}

uint32_t scheduler_get_thread_count(void) {
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    uint32_t cnt = 0;
    for (thread_t* t = g_thread_list; t; t = t->next) {
        if (t->state != THREAD_ZOMBIE) cnt++;
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
    return cnt;
}

void scheduler_yield_kernel(void) {
    __asm__ volatile ("int $0x81" : : : "memory");
}

static bool thread_eligible_for_cpu(const thread_t* t, uint32_t cpu_id) {
    if (!t || t->is_idle) return false;
    if (t->state != THREAD_READY) return false;
    if (t->running_cpu != -1) return false;
    if (t->pinned_cpu >= 0 && t->pinned_cpu != (int32_t)cpu_id) return false;
    return true;
}

static bool has_runnable_non_idle_locked(uint32_t cpu_id) {
    for (thread_t* t = g_thread_list; t; t = t->next) {
        if (thread_eligible_for_cpu(t, cpu_id)) return true;
    }
    return false;
}

bool scheduler_has_runnable_user(void) {
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    bool res = has_runnable_non_idle_locked(this_cpu_id());
    spin_unlock_irqrestore(&g_sched_lock, flags);
    return res;
}

static thread_t* choose_next_locked(uint32_t cpu_id, thread_t* after, bool skip_after) {
    /* High-priority desktop compositor always preempts normal tasks on CPU 0 */
    if (cpu_id == 0 && g_compositor_thread &&
        g_compositor_thread->state == THREAD_READY &&
        g_compositor_thread->running_cpu == -1) {
        if (g_compositor_thread != after || !skip_after) {
            return g_compositor_thread;
        }
    }

    thread_t* start = (after && after->next) ? after->next : g_thread_list;

    for (thread_t* t = start; t; t = t->next) {
        if (t != after && thread_eligible_for_cpu(t, cpu_id)) {
            return t;
        }
    }
    for (thread_t* t = g_thread_list; t && t != start; t = t->next) {
        if (t != after && thread_eligible_for_cpu(t, cpu_id)) {
            return t;
        }
    }

    if (!skip_after && after && !after->is_idle &&
        after->state == THREAD_READY &&
        (after->running_cpu == -1 || after->running_cpu == (int32_t)cpu_id) &&
        (after->pinned_cpu < 0 || after->pinned_cpu == (int32_t)cpu_id)) {
        return after;
    }

    thread_t* idle = (thread_t*)g_per_cpus[cpu_id].idle_thread;
    return (idle && (idle->state == THREAD_READY || idle == after)) ? idle : NULL;
}

static void switch_to_locked(per_cpu_t* cpu, thread_t* previous, thread_t* next) {
    if (!next || !cpu) return;

    if (previous != next) {
        if (previous) {
            if (previous->is_idle && previous->state == THREAD_RUNNING) {
                previous->state = THREAD_READY;
                if (previous->process) previous->process->state = PROCESS_READY;
            }
            fpu_save(previous->fpu_state);
            /* Defer releasing previous->running_cpu until RSP switches to next's stack */
            cpu->prev_release_ptr = &previous->running_cpu;
        } else {
            cpu->prev_release_ptr = NULL;
        }

        if (g_boot_switch_logs < 6 && cpu->cpu_id == 0) {
            g_boot_switch_logs++;
            log_printf("[SCHED] CPU0 switch pid %u -> %u, resume RIP=0x%016llx\n",
                       previous && previous->process ? previous->process->pid : 0,
                       next->process ? next->process->pid : 0,
                       next->resume_frame ? (unsigned long long)next->resume_frame->rip : 0);
        }

        cpu->current_process = next->process;
        cpu->current_thread  = (uint64_t)next;
        cpu->kernel_rsp0     = next->kernel_stack_top;
        tss_set_rsp0(next->kernel_stack_top);
        vmm_switch_pml4(next->process->pml4);

        /* Switch Ring 3 TLS FS_BASE MSR */
        if (next->is_user) {
            wrmsr(MSR_FS_BASE, next->fs_base);
        }

        fpu_restore(next->fpu_state);
    } else {
        cpu->prev_release_ptr = NULL;
    }

    next->running_cpu = (int32_t)cpu->cpu_id;
    next->state = THREAD_RUNNING;
    if (!next->is_idle) {
        if (next->process && next->process->state != PROCESS_ZOMBIE) {
            next->process->state = PROCESS_RUNNING;
        }
        if (next->quantum == 0) {
            next->quantum = next->is_compositor ? 5 : SCHEDULER_QUANTUM_TICKS;
        }
    }
}

void scheduler_start(void) {
    per_cpu_t* cpu = this_cpu();
    thread_t* idle = (thread_t*)cpu->idle_thread;
    if (!idle) {
        log_printf("[SCHED FATAL] BSP Idle thread is missing.\n");
        for (;;) __asm__ volatile ("pause");
    }

    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    cpu->current_thread = (uint64_t)idle;
    cpu->current_process = idle->process;
    cpu->kernel_rsp0 = idle->kernel_stack_top;
    idle->state = THREAD_RUNNING;
    idle->running_cpu = 0;
    tss_set_rsp0(idle->kernel_stack_top);
    cpu->need_resched = 0;
    __atomic_store_n(&g_scheduler_ready, true, __ATOMIC_RELEASE);

    thread_t* first = choose_next_locked(0, idle, true);
    if (!first) first = idle;

    switch_to_locked(cpu, idle, first);
    trap_frame_t* frame = first->resume_frame;
    spin_unlock_irqrestore(&g_sched_lock, flags);

    log_printf("[SCHED] Starting SMP preemptive scheduler (CPUs=%u, quantum=%u ms, compositor=PID %u).\n",
               smp_get_online_cpus(), SCHEDULER_QUANTUM_TICKS,
               g_compositor_thread && g_compositor_thread->process
                   ? g_compositor_thread->process->pid : 0);
    arch_enter_thread(frame);
}

void scheduler_start_ap(void) {
    per_cpu_t* cpu = this_cpu();
    thread_t* idle = (thread_t*)cpu->idle_thread;
    if (!idle) {
        for (;;) __asm__ volatile ("cli; hlt");
    }

    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    cpu->current_thread = (uint64_t)idle;
    cpu->current_process = idle->process;
    cpu->kernel_rsp0 = idle->kernel_stack_top;
    idle->state = THREAD_RUNNING;
    idle->running_cpu = (int32_t)cpu->cpu_id;
    tss_set_rsp0(idle->kernel_stack_top);
    cpu->need_resched = 0;

    thread_t* first = choose_next_locked(cpu->cpu_id, idle, true);
    if (!first) first = idle;

    switch_to_locked(cpu, idle, first);
    trap_frame_t* frame = first->resume_frame;
    spin_unlock_irqrestore(&g_sched_lock, flags);

    arch_enter_thread(frame);
}

void scheduler_idle_wait(void) {
    __asm__ volatile ("cli" : : : "memory");
    uint32_t cid = this_cpu_id();
    spin_lock(&g_sched_lock);
    bool has_work = has_runnable_non_idle_locked(cid);
    spin_unlock(&g_sched_lock);

    if (has_work) {
        __asm__ volatile ("sti" : : : "memory");
        scheduler_yield_kernel();
        return;
    }

    if (cid == 0 && !g_idle_reported) {
        g_idle_reported = true;
        log_printf("[SCHED] Idle threads sleeping with atomic STI;HLT window.\n");
    }

    __asm__ volatile ("sti; hlt" : : : "memory");
}

void scheduler_idle_entry(void) {
    for (;;) {
        scheduler_idle_wait();
    }
}

static bool all_proc_threads_reapable_locked(process_t* p) {
    for (thread_t* t = p->threads_head; t; t = t->proc_next) {
        if (t->state != THREAD_ZOMBIE || t->running_cpu != -1) {
            return false;
        }
    }
    return true;
}

static void unlink_thread_global_locked(thread_t* target) {
    thread_t** tp = &g_thread_list;
    thread_t* prev = NULL;
    while (*tp) {
        if (*tp == target) {
            *tp = target->next;
            if (g_last_thread == target) {
                g_last_thread = prev;
            }
            target->next = NULL;
            return;
        }
        prev = *tp;
        tp = &(*tp)->next;
    }
}

void scheduler_reap_zombies(void) {
    process_t* reap_procs = NULL;
    thread_t*  reap_subthreads = NULL;

    uint64_t flags = spin_lock_irqsave(&g_sched_lock);

    /* 1. Reap individual joined zombie sub-threads of still-alive processes */
    thread_t* t = g_thread_list;
    while (t) {
        thread_t* nxt = t->next;
        process_t* p = t->process;
        if (!t->is_idle && !t->is_compositor &&
            t->state == THREAD_ZOMBIE && t->running_cpu == -1 &&
            p && p->state != PROCESS_ZOMBIE &&
            t != p->main_thread && t->joined) {
            unlink_thread_global_locked(t);
            /* Unlink from process->threads_head */
            thread_t** pt = &p->threads_head;
            while (*pt) {
                if (*pt == t) {
                    *pt = t->proc_next;
                    t->proc_next = NULL;
                    if (p->thread_count > 0) p->thread_count--;
                    break;
                }
                pt = &(*pt)->proc_next;
            }
            t->next = reap_subthreads;
            reap_subthreads = t;
        }
        t = nxt;
    }

    /* 2. Reap fully exited zombie processes */
    process_t* p = process_head();
    while (p) {
        process_t* next_p = p->next;
        bool can_reap = false;
        if (p->pid != 0 && !p->is_system_daemon && p->state == PROCESS_ZOMBIE) {
            if (!p->parent || p->waited ||
                p->parent->state == PROCESS_ZOMBIE ||
                p->parent->is_system_daemon ||
                g_tick_count_100hz >= p->zombie_reap_tick) {
                if (all_proc_threads_reapable_locked(p)) {
                    can_reap = true;
                }
            }
        }
        if (can_reap) {
            for (thread_t* pt = p->threads_head; pt; pt = pt->proc_next) {
                unlink_thread_global_locked(pt);
            }
            process_unlink(p);
            p->next = reap_procs;
            reap_procs = p;
        }
        p = next_p;
    }

    spin_unlock_irqrestore(&g_sched_lock, flags);

    /* Free reaped sub-threads outside scheduler lock */
    while (reap_subthreads) {
        thread_t* st = reap_subthreads;
        reap_subthreads = st->next;
        if (st->kernel_stack_phys) {
            pmm_free_pages(st->kernel_stack_phys, st->kernel_stack_pages);
        }
        kfree(st);
    }

    /* Free reaped processes outside scheduler lock */
    while (reap_procs) {
        process_t* rp = reap_procs;
        reap_procs = rp->next;
        rp->next = NULL;

        process_cleanup_runtime_resources(rp);

        thread_t* pt = rp->threads_head;
        while (pt) {
            thread_t* next_pt = pt->proc_next;
            if (pt->kernel_stack_phys) {
                pmm_free_pages(pt->kernel_stack_phys, pt->kernel_stack_pages);
            }
            kfree(pt);
            pt = next_pt;
        }
        rp->threads_head = NULL;
        rp->main_thread = NULL;

        if (rp->r0_slot >= 0) {
            pxe_release_r0_image(rp);
        }

        if (rp->exec_ring == 3 && rp->pml4 && rp->pml4 != vmm_get_kernel_pml4()) {
            vmm_destroy_address_space(rp->pml4);
            rp->pml4 = NULL;
        }

        user_region_t* reg = rp->regions;
        while (reg) {
            user_region_t* next_reg = reg->next;
            kfree(reg);
            reg = next_reg;
        }
        rp->regions = NULL;

        kfree(rp);
    }
}

void scheduler_wake_compositor(void) {
    if (!g_scheduler_ready || !g_compositor_thread) return;
    scheduler_wake_thread(g_compositor_thread);
}

void scheduler_compositor_entry(void) {
    uint64_t next_frame_ns = cpu_monotonic_ns();
    for (;;) {
        usb_poll();
        audio_poll();
        ui_desktop_poll();
        audio_poll();
        scheduler_reap_zombies();

        if (g_acpi_shutdown_test_requested && g_tick_count_100hz >= 200) {
            g_acpi_shutdown_test_requested = false;
            log_printf("[ACPI TEST] Scheduler alive; starting direct S5 shutdown test\n");
            sys_shutdown();
        }

        int hz = gfx_accel_get_current_refresh_hz();
        if (hz < 60) hz = 60;
        if (hz > 240) hz = 240;
        uint64_t frame_ns = 1000000000ULL / (uint64_t)(unsigned int)hz;
        uint64_t now_ns = cpu_monotonic_ns();
        next_frame_ns += frame_ns;
        if (next_frame_ns <= now_ns || next_frame_ns - now_ns > frame_ns * 2ULL) {
            next_frame_ns = now_ns + frame_ns;
        }
        scheduler_block_current_ns(next_frame_ns);
        scheduler_yield_kernel();
    }
}

static void wake_sleepers_locked(uint64_t now_100hz, uint64_t now_ns) {
    for (thread_t* t = g_thread_list; t; t = t->next) {
        if (t->state != THREAD_BLOCKED) continue;
        bool wake = false;
        if (t->wake_tick != 0 && now_100hz >= t->wake_tick) {
            wake = true;
        } else if (t->wake_ns != 0 && now_ns >= t->wake_ns) {
            wake = true;
            if (t->futex_phys != 0) {
                t->futex_timed_out = true;
                if (t->resume_frame) {
                    t->resume_frame->rax = (uint64_t)(int64_t)(-NEKO_ETIMEDOUT);
                }
            }
        }
        if (wake) {
            t->wake_tick = 0;
            t->wake_ns = 0;
            t->wait_win_id = -1;
            t->futex_phys = 0;
            t->state = THREAD_READY;
            if (t->process && t->process->state != PROCESS_ZOMBIE) {
                t->process->state = PROCESS_READY;
            }
            send_resched_ipi_if_idle_locked(t->pinned_cpu);
        }
    }
}

trap_frame_t* scheduler_handle_irq(trap_frame_t* frame) {
    per_cpu_t* cpu = this_cpu();
    thread_t* current = (thread_t*)cpu->current_thread;
    if (!g_scheduler_ready || !current) return frame;

    /* Only perform timer accounting and preemption on Vector 32 (LAPIC Timer) */
    bool is_timer = (frame && frame->int_no == 32);

    if (is_timer) {
        /* Lock-free per-CPU 1000 Hz utilization counters */
        cpu->ticks_total++;
        if (current->is_idle) {
            cpu->ticks_idle++;
        } else if (frame->cs & 3) {
            cpu->ticks_user++;
        } else {
            cpu->ticks_kernel++;
        }

        if (current->process && !current->is_idle && current->state == THREAD_RUNNING) {
            __atomic_fetch_add(&current->process->cpu_ticks_total, 1ULL, __ATOMIC_RELAXED);
        }
    }

    uint64_t flags = spin_lock_irqsave(&g_sched_lock);

    if (is_timer && cpu->cpu_id == 0) {
        g_tick_count_1khz++;
        g_tick_count_100hz = apic_get_ticks();
        wake_sleepers_locked(g_tick_count_100hz, cpu_monotonic_ns());
    }

    if (current->state != THREAD_RUNNING) {
        if (current->state != THREAD_ZOMBIE) {
            current->resume_frame = frame;
        }
        thread_t* next = choose_next_locked(cpu->cpu_id, current, true);
        if (!next) {
            spin_unlock_irqrestore(&g_sched_lock, flags);
            return frame;
        }
        switch_to_locked(cpu, current, next);
        trap_frame_t* nf = next->resume_frame;
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return nf;
    }

    if (current->is_idle) {
        if (!has_runnable_non_idle_locked(cpu->cpu_id)) {
            spin_unlock_irqrestore(&g_sched_lock, flags);
            return frame;
        }
        current->resume_frame = frame;
        thread_t* next = choose_next_locked(cpu->cpu_id, current, true);
        if (!next || next == current) {
            spin_unlock_irqrestore(&g_sched_lock, flags);
            return frame;
        }
        switch_to_locked(cpu, current, next);
        trap_frame_t* nf = next->resume_frame;
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return nf;
    }

    /* Non-timer IRQs do not consume quantum or force preemption (except compositor wake on CPU0) */
    current->resume_frame = frame;
    if (current->wait_queue_id != 0) {
        current->wait_queue_id = 0;
    }

    if (cpu->cpu_id == 0 && !current->is_compositor && g_compositor_thread &&
        g_compositor_thread->state == THREAD_READY &&
        g_compositor_thread->running_cpu == -1) {
        current->state = THREAD_READY;
        if (current->process && current->process->state != PROCESS_ZOMBIE) {
            current->process->state = PROCESS_READY;
        }
        switch_to_locked(cpu, current, g_compositor_thread);
        trap_frame_t* nf = g_compositor_thread->resume_frame;
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return nf;
    }

    if (!is_timer) {
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return frame;
    }

    if (current->quantum > 0) current->quantum--;
    if (current->quantum > 0) {
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return frame;
    }

    current->state = THREAD_READY;
    if (current->process && current->process->state != PROCESS_ZOMBIE) {
        current->process->state = PROCESS_READY;
    }
    current->quantum = current->is_compositor ? 5 : SCHEDULER_QUANTUM_TICKS;

    thread_t* next = choose_next_locked(cpu->cpu_id, current, false);
    if (!next) next = current;
    if (next == current) {
        current->state = THREAD_RUNNING;
        if (current->process && current->process->state != PROCESS_ZOMBIE) {
            current->process->state = PROCESS_RUNNING;
        }
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return frame;
    }

    switch_to_locked(cpu, current, next);
    trap_frame_t* nf = next->resume_frame;
    spin_unlock_irqrestore(&g_sched_lock, flags);
    return nf;
}

trap_frame_t* scheduler_handle_ipi_resched(trap_frame_t* frame) {
    per_cpu_t* cpu = this_cpu();
    thread_t* current = (thread_t*)cpu->current_thread;
    if (!g_scheduler_ready || !current) return frame;

    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    if (current->is_idle && has_runnable_non_idle_locked(cpu->cpu_id)) {
        current->resume_frame = frame;
        thread_t* next = choose_next_locked(cpu->cpu_id, current, true);
        if (next && next != current) {
            switch_to_locked(cpu, current, next);
            trap_frame_t* nf = next->resume_frame;
            spin_unlock_irqrestore(&g_sched_lock, flags);
            return nf;
        }
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
    return frame;
}

trap_frame_t* scheduler_reschedule_from_syscall(trap_frame_t* frame) {
    per_cpu_t* cpu = this_cpu();
    thread_t* current = (thread_t*)cpu->current_thread;
    if (!current) return frame;

    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    if (current->state != THREAD_ZOMBIE) {
        current->resume_frame = frame;
    }
    if (current->state == THREAD_RUNNING) {
        current->state = THREAD_READY;
        if (current->process && current->process->state != PROCESS_ZOMBIE) {
            current->process->state = PROCESS_READY;
        }
    }

    cpu->need_resched = 0;
    thread_t* next = choose_next_locked(cpu->cpu_id, current, true);
    if (!next) {
        if (current->state != THREAD_ZOMBIE && current->state != THREAD_BLOCKED) {
            next = current;
        } else {
            next = (thread_t*)cpu->idle_thread;
        }
    }
    if (!next) {
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return frame;
    }

    switch_to_locked(cpu, current, next);
    trap_frame_t* nf = next->resume_frame;
    spin_unlock_irqrestore(&g_sched_lock, flags);
    return nf;
}

void scheduler_request_reschedule(void) {
    per_cpu_request_reschedule();
}

void scheduler_mark_current_ready(void) {
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    thread_t* cur = (thread_t*)this_cpu()->current_thread;
    if (cur && cur->state == THREAD_RUNNING) {
        cur->state = THREAD_READY;
        if (cur->process && cur->process->state != PROCESS_ZOMBIE) {
            cur->process->state = PROCESS_READY;
        }
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
}

void scheduler_block_current(uint64_t wake_tick) {
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    thread_t* cur = (thread_t*)this_cpu()->current_thread;
    if (cur) {
        cur->wake_tick = wake_tick;
        cur->wake_ns = 0;
        cur->state = THREAD_BLOCKED;
        if (cur->process && cur->process->alive_thread_count <= 1 &&
            cur->process->state != PROCESS_ZOMBIE) {
            cur->process->state = PROCESS_BLOCKED;
        }
        per_cpu_request_reschedule();
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
}

void scheduler_block_current_ns(uint64_t wake_ns) {
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    thread_t* cur = (thread_t*)this_cpu()->current_thread;
    if (cur) {
        cur->wake_tick = 0;
        cur->wake_ns = wake_ns;
        cur->state = THREAD_BLOCKED;
        if (cur->process && cur->process->alive_thread_count <= 1 &&
            cur->process->state != PROCESS_ZOMBIE) {
            cur->process->state = PROCESS_BLOCKED;
        }
        per_cpu_request_reschedule();
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
}

void scheduler_block_on_window(int32_t win_id, uint64_t timeout_ticks) {
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    thread_t* cur = (thread_t*)this_cpu()->current_thread;
    if (cur) {
        cur->wait_win_id = win_id;
        cur->wake_tick = timeout_ticks ? (g_tick_count_100hz + timeout_ticks) : 0;
        cur->wake_ns = 0;
        cur->state = THREAD_BLOCKED;
        if (cur->process && cur->process->alive_thread_count <= 1 &&
            cur->process->state != PROCESS_ZOMBIE) {
            cur->process->state = PROCESS_BLOCKED;
        }
        per_cpu_request_reschedule();
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
}

void scheduler_block_on_join(uint64_t target_tid) {
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    thread_t* cur = (thread_t*)this_cpu()->current_thread;
    if (cur) {
        cur->join_target_tid = target_tid;
        cur->wake_tick = 0;
        cur->wake_ns = 0;
        cur->state = THREAD_BLOCKED;
        per_cpu_request_reschedule();
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
}

void scheduler_wake_joiners(uint64_t exited_tid) {
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    for (thread_t* t = g_thread_list; t; t = t->next) {
        if (t->state == THREAD_BLOCKED && t->join_target_tid == exited_tid) {
            t->join_target_tid = 0;
            t->wake_tick = 0;
            t->wake_ns = 0;
            t->state = THREAD_READY;
            if (t->process && t->process->state != PROCESS_ZOMBIE) {
                t->process->state = PROCESS_READY;
            }
            send_resched_ipi_if_idle_locked(t->pinned_cpu);
        }
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
}

int scheduler_futex_wait(process_t* proc, uint64_t uaddr, uint32_t expected, uint64_t timeout_ns) {
    if (!proc || (uaddr & 3ULL) != 0 || !process_user_range_ok(proc, uaddr, 4, false)) {
        return -NEKO_EFAULT;
    }
    phys_addr_t phys = process_user_phys(proc, uaddr);
    if (!phys) return -NEKO_EFAULT;

    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    volatile uint32_t* kptr = (volatile uint32_t*)phys_to_virt(phys);
    uint32_t cur_val = __atomic_load_n(kptr, __ATOMIC_ACQUIRE);
    if (cur_val != expected) {
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return -NEKO_EAGAIN;
    }

    thread_t* cur = (thread_t*)this_cpu()->current_thread;
    if (!cur) {
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return -NEKO_EINVAL;
    }

    cur->futex_phys = phys;
    cur->futex_timed_out = false;
    cur->wake_tick = 0;
    cur->wake_ns = timeout_ns ? (cpu_monotonic_ns() + timeout_ns) : 0;
    cur->state = THREAD_BLOCKED;
    per_cpu_request_reschedule();
    spin_unlock_irqrestore(&g_sched_lock, flags);
    return 0;
}

int scheduler_futex_wake(process_t* proc, uint64_t uaddr, int max_wake) {
    if (!proc || (uaddr & 3ULL) != 0 || max_wake <= 0) {
        return 0;
    }
    phys_addr_t phys = process_user_phys(proc, uaddr);
    if (!phys) return -NEKO_EFAULT;

    int woken = 0;
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    for (thread_t* t = g_thread_list; t && woken < max_wake; t = t->next) {
        if (t->state == THREAD_BLOCKED && t->futex_phys == phys) {
            t->futex_phys = 0;
            t->futex_timed_out = false;
            t->wake_tick = 0;
            t->wake_ns = 0;
            t->state = THREAD_READY;
            if (t->process && t->process->state != PROCESS_ZOMBIE) {
                t->process->state = PROCESS_READY;
            }
            send_resched_ipi_if_idle_locked(t->pinned_cpu);
            woken++;
        }
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
    return woken;
}

void scheduler_wake_thread(thread_t* thread) {
    if (!thread) return;
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    if (thread->state == THREAD_BLOCKED) {
        thread->wake_tick = 0;
        thread->wake_ns = 0;
        thread->wait_queue_id = 0;
        thread->wait_win_id = -1;
        thread->futex_phys = 0;
        thread->state = THREAD_READY;
        if (thread->process && thread->process->state != PROCESS_ZOMBIE) {
            thread->process->state = PROCESS_READY;
        }
        send_resched_ipi_if_idle_locked(thread->pinned_cpu);
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
}

void scheduler_wake_win_waiters(int32_t win_id) {
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    for (thread_t* t = g_thread_list; t; t = t->next) {
        if (t->state == THREAD_BLOCKED &&
            (t->wait_win_id == win_id || t->wait_win_id == 0)) {
            t->wake_tick = 0;
            t->wake_ns = 0;
            t->wait_win_id = -1;
            t->state = THREAD_READY;
            if (t->process && t->process->state != PROCESS_ZOMBIE) {
                t->process->state = PROCESS_READY;
            }
            send_resched_ipi_if_idle_locked(t->pinned_cpu);
        }
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
}

thread_t* scheduler_find_ipc_waiter(uint64_t queue_id) {
    uint64_t flags = spin_lock_irqsave(&g_sched_lock);
    for (thread_t* t = g_thread_list; t; t = t->next) {
        if (t->state == THREAD_BLOCKED && t->wait_queue_id == queue_id) {
            spin_unlock_irqrestore(&g_sched_lock, flags);
            return t;
        }
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);
    return NULL;
}
