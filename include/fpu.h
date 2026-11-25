#ifndef _FPU_H
#define _FPU_H

#include <stdint.h>
#include <stdbool.h>

/* 1024 bytes (64-byte aligned) accommodates FXSAVE (512B) and XSAVE x87+SSE+AVX (832B) */
#define FPU_STATE_SIZE 1024

void fpu_init(void);
void fpu_init_ap(void);
bool fpu_xsave_enabled(void);
bool fpu_avx_enabled(void);
void fpu_init_state(void* state);
void fpu_save(void* state);
void fpu_restore(const void* state);

#endif /* _FPU_H */
