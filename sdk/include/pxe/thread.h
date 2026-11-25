#ifndef _PXE_THREAD_H
#define _PXE_THREAD_H

#include <stdint.h>
#include <stddef.h>

/*
 * Pixel Neko OS Ring 3 Multithreading & Synchronization API (<pxe/thread.h>)
 */

typedef uint64_t pxe_thread_t;

typedef struct {
    volatile uint32_t state; /* 0 = unlocked, 1 = locked (uncontended), 2 = locked (contended) */
} pxe_mutex_t;

typedef struct {
    volatile uint32_t seq;
} pxe_cond_t;

typedef struct {
    volatile uint32_t count;
} pxe_sem_t;

#define PXE_MUTEX_INIT { 0u }
#define PXE_COND_INIT  { 0u }
#define PXE_SEM_INIT(n) { (uint32_t)(n) }

/* Thread lifecycle */
int  pxe_thread_create(pxe_thread_t* out_tid, void* (*start_fn)(void*), void* arg);
int  pxe_thread_create_ex(pxe_thread_t* out_tid, void* (*start_fn)(void*), void* arg,
                          size_t stack_size, void* tls_base);
void pxe_thread_exit(int status) __attribute__((noreturn));
int  pxe_thread_join(pxe_thread_t tid, int* out_status);

/* Low-level Futex primitives */
int  pxe_futex_wait(volatile uint32_t* addr, uint32_t expected, uint64_t timeout_ns);
int  pxe_futex_wake(volatile uint32_t* addr, int max_wake);

/* Mutex */
void pxe_mutex_init(pxe_mutex_t* mtx);
void pxe_mutex_lock(pxe_mutex_t* mtx);
int  pxe_mutex_trylock(pxe_mutex_t* mtx);
void pxe_mutex_unlock(pxe_mutex_t* mtx);

/* Condition variable */
void pxe_cond_init(pxe_cond_t* cond);
int  pxe_cond_wait(pxe_cond_t* cond, pxe_mutex_t* mtx);
int  pxe_cond_timedwait(pxe_cond_t* cond, pxe_mutex_t* mtx, uint64_t timeout_ns);
void pxe_cond_signal(pxe_cond_t* cond);
void pxe_cond_broadcast(pxe_cond_t* cond);

/* Counting Semaphore */
void pxe_sem_init(pxe_sem_t* sem, uint32_t initial_count);
void pxe_sem_wait(pxe_sem_t* sem);
int  pxe_sem_trywait(pxe_sem_t* sem);
void pxe_sem_post(pxe_sem_t* sem);

#endif /* _PXE_THREAD_H */
