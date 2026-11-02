#ifndef _FPU_H
#define _FPU_H

#include <stdint.h>

#define FPU_STATE_SIZE 512

void fpu_init(void);
void fpu_init_state(void* state);
void fpu_save(void* state);
void fpu_restore(const void* state);

#endif /* _FPU_H */
