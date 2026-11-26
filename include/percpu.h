#ifndef _PERCPU_H
#define _PERCPU_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SMP_MAX_CPUS 128u

#define CPU_CORE_TYPE_GENERIC 0u
#define CPU_CORE_TYPE_PERF    1u /* Intel P-Core (0x40) */
#define CPU_CORE_TYPE_EFF     2u /* Intel E-Core (0x20) */

/*
 * Per-CPU control block addressed via %gs in kernel mode.
 * Offsets 0..64 are part of the assembly ABI (syscall_entry.asm, nmi.asm,
 * interrupts.asm) and MUST NOT be reordered.
 */
typedef struct per_cpu {
    /* Fixed assembly ABI (offsets 0..56) */
    uint64_t kernel_rsp0;      /* +0  */
    uint64_t user_rsp;         /* +8  */
    uint64_t need_resched;     /* +16 */
    uint64_t switch_gs;        /* +24 */
    uint64_t current_thread;   /* +32 */
    uint64_t nmi_swapgs;       /* +40 */
    uint64_t syscall_count;    /* +48 */
    uint64_t nmi_count;        /* +56 */

    /* Fast self-pointer for this_cpu() (+64) */
    struct per_cpu* self;      /* +64 */

    /* SMP identity & hardware topology */
    uint32_t cpu_id;           /* +72: logical index 0..SMP_MAX_CPUS-1 */
    uint32_t lapic_id;         /* +76: xAPIC or x2APIC ID */
    uint32_t package_id;       /* +80: physical socket / package */
    uint32_t core_id;          /* +84: physical core ID within package */
    uint32_t smt_id;           /* +88: hardware thread index within core */
    uint8_t  core_type;        /* +92: CPU_CORE_TYPE_* */
    uint8_t  online;           /* +93: 1 if CPU is online */
    uint8_t  in_irq;           /* +94 */
    volatile uint8_t resched_ipi_pending; /* +95: 1 if resched IPI in flight */

    /* Per-CPU scheduler & TSS bindings */
    void*    idle_thread;      /* +96  */
    void*    current_process;  /* +104 */
    uint64_t tss_base;         /* +112 */
    uint32_t preempt_count;    /* +120 */
    uint32_t irq_depth;        /* +124 */

    /* Lock-free per-CPU 1000 Hz utilization counters (written only by local timer IRQ) */
    volatile uint64_t ticks_total;  /* +128 */
    volatile uint64_t ticks_idle;   /* +136 */
    volatile uint64_t ticks_kernel; /* +144 */
    volatile uint64_t ticks_user;   /* +152 */

    /* Deferred previous-thread pointer finalized after RSP switches to next thread (+160) */
    void* prev_switch_thread;          /* +160 */

    uint64_t _reserved[3];          /* pad to 192 bytes (3 cache lines) */
} __attribute__((aligned(64))) per_cpu_t;

extern per_cpu_t g_per_cpus[SMP_MAX_CPUS];
extern volatile uint64_t per_cpu_nmi_test_pending;

static inline per_cpu_t* this_cpu(void) {
    per_cpu_t* ptr;
    __asm__ volatile ("mov %%gs:64, %0" : "=r"(ptr));
    return ptr;
}

static inline uint32_t this_cpu_id(void) {
    return this_cpu()->cpu_id;
}

void per_cpu_init_bsp(void);
void per_cpu_init_ap(uint32_t cpu_id, uint32_t lapic_id);
void per_cpu_init(void* percpu);
void per_cpu_set_kernel_stack(uint64_t rsp0);
void per_cpu_set_current(void* thread);
void per_cpu_set_gs_domain(bool user);
void per_cpu_request_reschedule(void);
bool per_cpu_reschedule_requested(void);
void per_cpu_clear_reschedule(void);

#endif /* _PERCPU_H */
