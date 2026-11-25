#include "../../include/neko_abi.h"
#include <pxe/thread.h>
#include <pxe/mem.h>
#include <pxe/sys.h>

#define PXE_THREAD_DEFAULT_STACK (64u * 1024u)

static inline long _thr_syscall1(long num, long a0) {
    long ret = num;
    __asm__ volatile ("syscall" : "+a"(ret) : "D"(a0) : "rcx", "r11", "memory");
    return ret;
}

static inline long _thr_syscall2(long num, long a0, long a1) {
    long ret = num;
    __asm__ volatile ("syscall" : "+a"(ret) : "D"(a0), "S"(a1) : "rcx", "r11", "memory");
    return ret;
}

static inline long _thr_syscall3(long num, long a0, long a1, long a2) {
    long ret = num;
    __asm__ volatile ("syscall" : "+a"(ret) : "D"(a0), "S"(a1), "d"(a2) : "rcx", "r11", "memory");
    return ret;
}

static inline long _thr_syscall4(long num, long a0, long a1, long a2, long a3) {
    register long r10 __asm__("r10") = a3;
    long ret = num;
    __asm__ volatile ("syscall" : "+a"(ret) : "D"(a0), "S"(a1), "d"(a2), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}

typedef struct {
    void* (*start_fn)(void*);
    void* arg;
    void* stack_base;
} pxe_thread_tramp_arg_t;

static void _pxe_thread_trampoline(void* raw) {
    pxe_thread_tramp_arg_t* ta = (pxe_thread_tramp_arg_t*)raw;
    void* (*fn)(void*) = ta->start_fn;
    void* arg = ta->arg;
    void* ret = fn ? fn(arg) : (void*)0;
    pxe_thread_exit((int)(intptr_t)ret);
}

int pxe_thread_create_ex(pxe_thread_t* out_tid, void* (*start_fn)(void*), void* arg,
                         size_t stack_size, void* tls_base) {
    if (!start_fn) return -NEKO_EINVAL;
    if (stack_size < 16384u) stack_size = PXE_THREAD_DEFAULT_STACK;
    stack_size = (stack_size + 4095u) & ~((size_t)4095u);

    uint8_t* stack = (uint8_t*)malloc(stack_size);
    if (!stack) return -NEKO_ENOMEM;

    /* Place trampoline arguments near the top of the new stack */
    uintptr_t top = ((uintptr_t)stack + stack_size - sizeof(pxe_thread_tramp_arg_t)) & ~((uintptr_t)15u);
    pxe_thread_tramp_arg_t* ta = (pxe_thread_tramp_arg_t*)top;
    ta->start_fn   = start_fn;
    ta->arg        = arg;
    ta->stack_base = stack;

    /* System V AMD64 ABI expects RSP % 16 == 8 on function entry (as if CALL pushed return addr) */
    uintptr_t rsp_entry = ((top - 16u) & ~((uintptr_t)15u)) - 8u;
    *(uint64_t*)rsp_entry = 0ULL;

    long tid = _thr_syscall4(NEKO_SYSCALL_THREAD_CREATE,
                             (long)(uintptr_t)_pxe_thread_trampoline,
                             (long)(uintptr_t)ta,
                             (long)rsp_entry,
                             (long)(uintptr_t)tls_base);
    if (tid < 0) {
        free(stack);
        return (int)tid;
    }
    if (out_tid) *out_tid = (pxe_thread_t)tid;
    return 0;
}

int pxe_thread_create(pxe_thread_t* out_tid, void* (*start_fn)(void*), void* arg) {
    return pxe_thread_create_ex(out_tid, start_fn, arg, PXE_THREAD_DEFAULT_STACK, (void*)0);
}

void pxe_thread_exit(int status) {
    (void)_thr_syscall1(NEKO_SYSCALL_THREAD_EXIT, (long)status);
    for (;;) {
        __asm__ volatile ("pause");
    }
}

int pxe_thread_join(pxe_thread_t tid, int* out_status) {
    int rc;
    do {
        rc = (int)_thr_syscall2(NEKO_SYSCALL_THREAD_JOIN, (long)tid, (long)(uintptr_t)out_status);
    } while (rc == -NEKO_EAGAIN);
    return rc;
}

int pxe_futex_wait(volatile uint32_t* addr, uint32_t expected, uint64_t timeout_ns) {
    return (int)_thr_syscall3(NEKO_SYSCALL_FUTEX_WAIT,
                              (long)(uintptr_t)addr,
                              (long)expected,
                              (long)timeout_ns);
}

int pxe_futex_wake(volatile uint32_t* addr, int max_wake) {
    return (int)_thr_syscall2(NEKO_SYSCALL_FUTEX_WAKE,
                              (long)(uintptr_t)addr,
                              (long)max_wake);
}

/* =========================================================================
 * Futex-based Mutex (3-state Ulrich Drepper algorithm)
 * ========================================================================= */

void pxe_mutex_init(pxe_mutex_t* mtx) {
    if (mtx) __atomic_store_n(&mtx->state, 0u, __ATOMIC_RELAXED);
}

int pxe_mutex_trylock(pxe_mutex_t* mtx) {
    if (!mtx) return -1;
    uint32_t expected = 0u;
    return __atomic_compare_exchange_n(&mtx->state, &expected, 1u, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 0 : -1;
}

void pxe_mutex_lock(pxe_mutex_t* mtx) {
    if (!mtx) return;
    uint32_t expected = 0u;
    if (__atomic_compare_exchange_n(&mtx->state, &expected, 1u, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return;
    }
    for (;;) {
        if (expected == 2u ||
            __atomic_compare_exchange_n(&mtx->state, &expected, 2u, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            (void)pxe_futex_wait(&mtx->state, 2u, 0ULL);
        }
        expected = 0u;
        if (__atomic_compare_exchange_n(&mtx->state, &expected, 2u, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return;
        }
    }
}

void pxe_mutex_unlock(pxe_mutex_t* mtx) {
    if (!mtx) return;
    uint32_t prev = __atomic_fetch_sub(&mtx->state, 1u, __ATOMIC_RELEASE);
    if (prev != 1u) {
        __atomic_store_n(&mtx->state, 0u, __ATOMIC_RELEASE);
        (void)pxe_futex_wake(&mtx->state, 1);
    }
}

/* =========================================================================
 * Condition Variable
 * ========================================================================= */

void pxe_cond_init(pxe_cond_t* cond) {
    if (cond) __atomic_store_n(&cond->seq, 0u, __ATOMIC_RELAXED);
}

int pxe_cond_timedwait(pxe_cond_t* cond, pxe_mutex_t* mtx, uint64_t timeout_ns) {
    if (!cond || !mtx) return -NEKO_EINVAL;
    uint32_t seq = __atomic_load_n(&cond->seq, __ATOMIC_RELAXED);
    pxe_mutex_unlock(mtx);
    int rc = pxe_futex_wait(&cond->seq, seq, timeout_ns);
    pxe_mutex_lock(mtx);
    return rc;
}

int pxe_cond_wait(pxe_cond_t* cond, pxe_mutex_t* mtx) {
    return pxe_cond_timedwait(cond, mtx, 0ULL);
}

void pxe_cond_signal(pxe_cond_t* cond) {
    if (!cond) return;
    __atomic_fetch_add(&cond->seq, 1u, __ATOMIC_RELEASE);
    (void)pxe_futex_wake(&cond->seq, 1);
}

void pxe_cond_broadcast(pxe_cond_t* cond) {
    if (!cond) return;
    __atomic_fetch_add(&cond->seq, 1u, __ATOMIC_RELEASE);
    (void)pxe_futex_wake(&cond->seq, 0x7FFFFFFF);
}

/* =========================================================================
 * Counting Semaphore
 * ========================================================================= */

void pxe_sem_init(pxe_sem_t* sem, uint32_t initial_count) {
    if (sem) __atomic_store_n(&sem->count, initial_count, __ATOMIC_RELAXED);
}

int pxe_sem_trywait(pxe_sem_t* sem) {
    if (!sem) return -1;
    uint32_t cur = __atomic_load_n(&sem->count, __ATOMIC_RELAXED);
    while (cur > 0u) {
        if (__atomic_compare_exchange_n(&sem->count, &cur, cur - 1u, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return 0;
        }
    }
    return -1;
}

void pxe_sem_wait(pxe_sem_t* sem) {
    if (!sem) return;
    for (;;) {
        if (pxe_sem_trywait(sem) == 0) return;
        (void)pxe_futex_wait(&sem->count, 0u, 0ULL);
    }
}

void pxe_sem_post(pxe_sem_t* sem) {
    if (!sem) return;
    uint32_t prev = __atomic_fetch_add(&sem->count, 1u, __ATOMIC_RELEASE);
    if (prev == 0u) {
        (void)pxe_futex_wake(&sem->count, 1);
    }
}
