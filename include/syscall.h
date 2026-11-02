#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <stdint.h>
#include <stdbool.h>
#include "idt.h"

#define SYSCALL_VECTOR_MARKER 0x100

void syscall_init(void);
int64_t syscall_dispatcher(trap_frame_t* frame);
trap_frame_t* syscall_reschedule(trap_frame_t* frame);
trap_frame_t* syscall_bad_return(trap_frame_t* frame);
void c_nmi_handler(trap_frame_t* frame);
uint64_t syscall_get_count(void);
uint64_t nmi_get_count(void);
void syscall_mark_reschedule(void);
bool syscall_reschedule_requested(void);

extern void syscall_entry(void);
extern void nmi_entry(void);
extern void arch_enter_thread(trap_frame_t* frame) __attribute__((noreturn));
extern void arch_restore_trap_frame(void) __attribute__((noreturn));

#endif /* _SYSCALL_H */
