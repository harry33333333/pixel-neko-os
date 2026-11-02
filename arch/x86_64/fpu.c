#include "fpu.h"
#include "string.h"

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

void fpu_init(void) {
    uint64_t cr0 = read_cr0();
    cr0 |= (1ULL << 1);       // MP: monitor coprocessor
    cr0 |= (1ULL << 5);       // NE: native x87/SIMD exception reporting
    cr0 &= ~(1ULL << 2);      // EM: no FPU emulation
    cr0 &= ~(1ULL << 3);      // TS: FPU context is currently live
    write_cr0(cr0);

    uint64_t cr4 = read_cr4();
    cr4 |= (1ULL << 9);       // OSFXSR: FXSAVE/FXRSTOR enabled
    cr4 |= (1ULL << 10);      // OSXMMEXCPT: unmasked SIMD exceptions
    write_cr4(cr4);

    __asm__ volatile ("fninit" : : : "memory");
}

void fpu_init_state(void* state) {
    if (!state) return;
    memset(state, 0, FPU_STATE_SIZE);

    uint8_t* bytes = (uint8_t*)state;
    *(uint16_t*)(bytes + 0) = 0x037F;   // FCW
    *(uint32_t*)(bytes + 24) = 0x1F80;  // MXCSR: all exceptions masked
}
