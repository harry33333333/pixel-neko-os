#ifndef _PERCPU_H
#define _PERCPU_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * These symbols live in arch/x86_64/syscall_entry.asm so the SYSCALL fast
 * path can address them with fixed GS offsets. The first twelve fields are
 * part of that assembly ABI and must not be reordered.
 */
typedef struct {
    uint64_t kernel_rsp0;
    uint64_t user_rsp;
    uint64_t need_resched;
    uint64_t switch_gs;
    uint64_t current_thread;
    uint64_t nmi_swapgs;
    uint64_t syscall_count;
    uint64_t nmi_count;
} per_cpu_t;

extern per_cpu_t per_cpu_block;
extern uint64_t per_cpu_kernel_rsp0;
extern uint64_t per_cpu_user_rsp;
extern uint64_t per_cpu_need_resched;
extern uint64_t per_cpu_switch_gs;
extern uint64_t per_cpu_current_thread;
extern uint64_t per_cpu_nmi_count;
extern uint64_t per_cpu_nmi_test_pending;

void per_cpu_init(void* percpu);
void per_cpu_set_kernel_stack(uint64_t rsp0);
void per_cpu_set_current(void* thread);
void per_cpu_set_gs_domain(bool user);
void per_cpu_request_reschedule(void);
bool per_cpu_reschedule_requested(void);
void per_cpu_clear_reschedule(void);

#endif /* _PERCPU_H */
