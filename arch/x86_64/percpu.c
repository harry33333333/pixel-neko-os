#include "percpu.h"
#include "cpu.h"
#include <stddef.h>

_Static_assert(sizeof(per_cpu_t) == 64, "per_cpu_t must stay assembly-compatible");
_Static_assert(offsetof(per_cpu_t, kernel_rsp0) == 0, "kernel_rsp0 offset");
_Static_assert(offsetof(per_cpu_t, user_rsp) == 8, "user_rsp offset");
_Static_assert(offsetof(per_cpu_t, need_resched) == 16, "need_resched offset");
_Static_assert(offsetof(per_cpu_t, switch_gs) == 24, "switch_gs offset");
_Static_assert(offsetof(per_cpu_t, current_thread) == 32, "current_thread offset");
_Static_assert(offsetof(per_cpu_t, nmi_swapgs) == 40, "nmi_swapgs offset");
_Static_assert(offsetof(per_cpu_t, syscall_count) == 48, "syscall_count offset");
_Static_assert(offsetof(per_cpu_t, nmi_count) == 56, "nmi_count offset");

void per_cpu_init(void* percpu) {
    if (!percpu) percpu = &per_cpu_block;

    wrmsr(MSR_GS_BASE, (uint64_t)percpu);
    wrmsr(MSR_KERNEL_GS_BASE, 0);

    per_cpu_block.kernel_rsp0 = 0;
    per_cpu_block.user_rsp = 0;
    per_cpu_block.need_resched = 0;
    per_cpu_block.switch_gs = 0;
    per_cpu_block.current_thread = 0;
    per_cpu_block.nmi_swapgs = 0;
    per_cpu_block.syscall_count = 0;
    per_cpu_block.nmi_count = 0;
}

void per_cpu_set_kernel_stack(uint64_t rsp0) {
    per_cpu_block.kernel_rsp0 = rsp0;
}

void per_cpu_set_current(void* thread) {
    per_cpu_block.current_thread = (uint64_t)thread;
}

void per_cpu_request_reschedule(void) {
    per_cpu_block.need_resched = 1;
}

bool per_cpu_reschedule_requested(void) {
    return per_cpu_block.need_resched != 0;
}

void per_cpu_clear_reschedule(void) {
    per_cpu_block.need_resched = 0;
}
