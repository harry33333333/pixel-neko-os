#ifndef _CPU_H
#define _CPU_H

#include <stdint.h>
#include <stdbool.h>

// 1. CPU Extended Features (EAX = 0x80000001)
#define CPUID_EXT_SYSCALL   (1u << 11)   // EDX Bit 11: 64-bit SYSCALL / SYSRET support
#define CPUID_EXT_NX        (1u << 20)   // EDX Bit 20: No-Execute page protection
#define CPUID_EXT_LM        (1u << 29)   // EDX Bit 29: Long Mode (64-bit)

// 2. CPU Basic Features (EAX = 0x00000001)
#define CPUID_FEAT_APIC     (1u << 9)    // EDX Bit 9:  On-chip Local APIC
#define CPUID_FEAT_SSE2     (1u << 26)   // EDX Bit 26: SSE2 instructions
#define CPUID_FEAT_SSE3     (1u << 0)    // ECX Bit 0:  SSE3 instructions
#define CPUID_FEAT_CX16     (1u << 13)   // ECX Bit 13: CMPXCHG16B (128-bit atomic)

// Common MSRs
#define MSR_IA32_APIC_BASE  0x0000001B
#define MSR_IA32_EFER       0xC0000080
#define MSR_IA32_STAR       0xC0000081
#define MSR_IA32_LSTAR      0xC0000082
#define MSR_IA32_CSTAR      0xC0000083
#define MSR_IA32_FMASK      0xC0000084
#define MSR_FS_BASE         0xC0000100
#define MSR_GS_BASE         0xC0000101
#define MSR_KERNEL_GS_BASE  0xC0000102

// EFER bits
#define EFER_SCE            (1ULL << 0)  // System Call Extensions
#define EFER_LME            (1ULL << 8)  // Long Mode Enable
#define EFER_LMA            (1ULL << 10) // Long Mode Active
#define EFER_NXE            (1ULL << 11) // No-Execute Enable

typedef struct {
    char vendor[13];
    char brand[49];
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
    uint32_t mhz;
    uint32_t max_basic_leaf;
    uint32_t max_ext_leaf;
    uint32_t basic_ecx;
    uint32_t basic_edx;
    uint32_t ext_ecx;
    uint32_t ext_edx;
    bool has_lm;
    bool has_nx;
    bool has_apic;
    bool has_syscall;
    bool has_sse2;
    bool has_sse3;
    bool has_cx16;
} cpu_info_t;

extern cpu_info_t g_cpu_info;
extern char cpu_model_string[49];

static inline void cpuid(uint32_t leaf, uint32_t subleaf,
                         uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid"
                      : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                      : "a"(leaf), "c"(subleaf));
    if (eax) *eax = a;
    if (ebx) *ebx = b;
    if (ecx) *ecx = c;
    if (edx) *edx = d;
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low = (uint32_t)val;
    uint32_t high = (uint32_t)(val >> 32);
    __asm__ volatile ("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}

bool cpu_detect_and_verify(void);

#endif /* _CPU_H */
