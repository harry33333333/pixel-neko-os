#ifndef _SCHEDULER_H
#define _SCHEDULER_H

#include <stdint.h>
#include <stdbool.h>
#include "process.h"

#define SCHEDULER_QUANTUM_TICKS 5

void scheduler_init(void);
void scheduler_add_thread(thread_t* thread);
void scheduler_start(void) __attribute__((noreturn));
void scheduler_idle_entry(void) __attribute__((noreturn));
void scheduler_compositor_entry(void) __attribute__((noreturn));
void scheduler_idle_wait(void);
void scheduler_yield_kernel(void);
void scheduler_reap_zombies(void);
trap_frame_t* scheduler_handle_irq(trap_frame_t* frame);
trap_frame_t* scheduler_reschedule_from_syscall(trap_frame_t* frame);
void scheduler_request_reschedule(void);
void scheduler_mark_current_ready(void);
void scheduler_block_current(uint64_t wake_tick);
void scheduler_block_on_window(int32_t win_id, uint64_t timeout_ticks);
void scheduler_wake_thread(thread_t* thread);
void scheduler_wake_win_waiters(int32_t win_id);
thread_t* scheduler_find_ipc_waiter(uint64_t queue_id);
bool scheduler_has_runnable_user(void);
uint64_t scheduler_get_ticks(void);
thread_t* scheduler_current_thread(void);

#endif /* _SCHEDULER_H */
