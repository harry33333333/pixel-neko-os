#include "percpu.h"
#include "cpu.h"
#include "string.h"
#include <stddef.h>

_Static_assert(offsetof(per_cpu_t, kernel_rsp0) == 0, "kernel_rsp0 offset");
_Static_assert(offsetof(per_cpu_t, user_rsp) == 8, "user_rsp offset");
_Static_assert(offsetof(per_cpu_t, need_resched) == 16, "need_resched offset");
_Static_assert(offsetof(per_cpu_t, switch_gs) == 24, "switch_gs offset");
_Static_assert(offsetof(per_cpu_t, current_thread) == 32, "current_thread offset");
_Static_assert(offsetof(per_cpu_t, nmi_swapgs) == 40, "nmi_swapgs offset");
_Static_assert(offsetof(per_cpu_t, syscall_count) == 48, "syscall_count offset");
_Static_assert(offsetof(per_cpu_t, nmi_count) == 56, "nmi_count offset");
_Static_assert(offsetof(per_cpu_t, self) == 64, "self offset");
_Static_assert(offsetof(per_cpu_t, prev_switch_thread) == 160, "prev_switch_thread offset");
_Static_assert((sizeof(per_cpu_t) % 64) == 0, "per_cpu_t must be cache-line aligned");

__attribute__((aligned(64)))
per_cpu_t g_per_cpus[SMP_MAX_CPUS];

static bool g_bsp_percpu_ready = false;

void per_cpu_init_bsp(void) {
    per_cpu_t* bsp = &g_per_cpus[0];
    if (!g_bsp_percpu_ready) {
        uint64_t saved_tss = bsp->tss_base;
        memset(g_per_cpus, 0, sizeof(g_per_cpus));
        bsp->self = bsp;
        bsp->cpu_id = 0;
        bsp->online = 1;
        bsp->tss_base = saved_tss;
        per_cpu_nmi_test_pending = 0;
        g_bsp_percpu_ready = true;
    }
    wrmsr(MSR_GS_BASE, (uint64_t)bsp);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}

void per_cpu_init_ap(uint32_t cpu_id, uint32_t lapic_id) {
    if (cpu_id >= SMP_MAX_CPUS) return;
    per_cpu_t* cpu = &g_per_cpus[cpu_id];
    /* Preserve idle_thread & tss_base if already populated before AP init */
    void* saved_idle = cpu->idle_thread;
    uint64_t saved_tss = cpu->tss_base;
    memset(cpu, 0, sizeof(*cpu));
    cpu->self = cpu;
    cpu->cpu_id = cpu_id;
    cpu->lapic_id = lapic_id;
    cpu->idle_thread = saved_idle;
    cpu->tss_base = saved_tss;
    wrmsr(MSR_GS_BASE, (uint64_t)cpu);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}

void per_cpu_init(void* percpu) {
    if (!percpu) {
        per_cpu_init_bsp();
        return;
    }
    per_cpu_t* cpu = (per_cpu_t*)percpu;
    cpu->self = cpu;
    wrmsr(MSR_GS_BASE, (uint64_t)cpu);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}

void per_cpu_set_kernel_stack(uint64_t rsp0) {
    this_cpu()->kernel_rsp0 = rsp0;
}

void per_cpu_set_current(void* thread) {
    this_cpu()->current_thread = (uint64_t)thread;
}

void per_cpu_set_gs_domain(bool user) {
    per_cpu_t* cpu = this_cpu();
    if (user) {
        wrmsr(MSR_GS_BASE, 0);
        wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)cpu);
    } else {
        wrmsr(MSR_GS_BASE, (uint64_t)cpu);
        wrmsr(MSR_KERNEL_GS_BASE, 0);
    }
    cpu->switch_gs = 0;
}

void per_cpu_request_reschedule(void) {
    this_cpu()->need_resched = 1;
}

bool per_cpu_reschedule_requested(void) {
    return this_cpu()->need_resched != 0;
}

void per_cpu_clear_reschedule(void) {
    this_cpu()->need_resched = 0;
}
