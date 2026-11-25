#include "fpu.h"
#include "cpu.h"
#include "log.h"
#include "string.h"

volatile uint32_t g_fpu_use_xsave = 0;
volatile uint64_t g_fpu_xsave_mask = 0x3ULL; /* x87 | SSE */
static bool g_fpu_avx_active = false;

static inline uint64_t read_cr0(void) {
    uint64_t value;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(value));
    return value;
}

static inline void write_cr0(uint64_t value) {
    __asm__ volatile ("mov %0, %%cr0" : : "r"(value) : "memory");
}

static inline uint64_t read_cr4(void) {
    uint64_t value;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(value));
    return value;
}

static inline void write_cr4(uint64_t value) {
    __asm__ volatile ("mov %0, %%cr4" : : "r"(value) : "memory");
}

static inline void write_xcr0(uint64_t val) {
    uint32_t lo = (uint32_t)(val & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile ("xsetbv" : : "c"(0u), "a"(lo), "d"(hi) : "memory");
}

static void fpu_enable_base(bool enable_xsave, uint64_t xcr0_val) {
    uint64_t cr0 = read_cr0();
    cr0 |= (1ULL << 1);       // MP: monitor coprocessor
    cr0 |= (1ULL << 5);       // NE: native x87/SIMD exception reporting
    cr0 &= ~(1ULL << 2);      // EM: no FPU emulation
    cr0 &= ~(1ULL << 3);      // TS: FPU context is currently live
    write_cr0(cr0);

    uint64_t cr4 = read_cr4();
    cr4 |= (1ULL << 9);       // OSFXSR: FXSAVE/FXRSTOR enabled
    cr4 |= (1ULL << 10);      // OSXMMEXCPT: unmasked SIMD exceptions
    if (enable_xsave) {
        cr4 |= (1ULL << 18);  // OSXSAVE: enable XSAVE/XRSTOR/XSETBV
    }
    write_cr4(cr4);

    if (enable_xsave) {
        write_xcr0(xcr0_val);
    }

    __asm__ volatile ("fninit" : : : "memory");
}

void fpu_init(void) {
    bool use_xsave = false;
    uint64_t xcr0_mask = 0x3ULL; /* x87 (bit 0) + SSE (bit 1) */

    if (g_cpu_info.has_xsave && g_cpu_info.max_basic_leaf >= 0x0D) {
        uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
        cpuid(0x0D, 0, &eax, &ebx, &ecx, &edx);
        uint64_t supported = ((uint64_t)edx << 32) | (uint64_t)eax;
        if ((supported & 0x3ULL) == 0x3ULL) {
            uint64_t target = 0x3ULL;
            if (g_cpu_info.has_avx && (supported & 0x4ULL)) {
                target |= 0x4ULL; /* AVX YMM state */
            }
            /* Temporarily enable OSXSAVE to verify area size with target XCR0 */
            uint64_t cr4 = read_cr4();
            write_cr4(cr4 | (1ULL << 9) | (1ULL << 10) | (1ULL << 18));
            write_xcr0(target);
            cpuid(0x0D, 0, &eax, &ebx, &ecx, &edx);
            if (ebx > 0 && ebx <= FPU_STATE_SIZE) {
                use_xsave = true;
                xcr0_mask = target;
            } else {
                write_xcr0(0x3ULL);
                write_cr4(cr4);
            }
        }
    }

    g_fpu_use_xsave = use_xsave ? 1u : 0u;
    g_fpu_xsave_mask = xcr0_mask;
    g_fpu_avx_active = use_xsave && ((xcr0_mask & 0x4ULL) != 0);

    fpu_enable_base(use_xsave, xcr0_mask);
    log_printf("[FPU] %s active (XCR0=0x%llx, AVX=%s, AVX2=%s, state=%u bytes).\n",
               use_xsave ? "XSAVE64/XRSTOR64" : "FXSAVE64/FXRSTOR64",
               (unsigned long long)xcr0_mask,
               g_fpu_avx_active ? "ON" : "OFF",
               (g_fpu_avx_active && g_cpu_info.has_avx2) ? "ON" : "OFF",
               (unsigned)FPU_STATE_SIZE);
}

void fpu_init_ap(void) {
    fpu_enable_base(g_fpu_use_xsave != 0, g_fpu_xsave_mask);
}

bool fpu_xsave_enabled(void) {
    return g_fpu_use_xsave != 0;
}

bool fpu_avx_enabled(void) {
    return g_fpu_avx_active;
}

void fpu_init_state(void* state) {
    if (!state) return;
    memset(state, 0, FPU_STATE_SIZE);

    uint8_t* bytes = (uint8_t*)state;
    *(uint16_t*)(bytes + 0) = 0x037F;   // FCW
    *(uint32_t*)(bytes + 24) = 0x1F80;  // MXCSR: all exceptions masked
    /* XSAVE header at offset 512..575 is zeroed (init state for all components) */
}
