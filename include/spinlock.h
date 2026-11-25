#ifndef _SPINLOCK_H
#define _SPINLOCK_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Fair Ticket Spinlock for SMP synchronization.
 * Always use spin_lock_irqsave/spin_unlock_irqrestore when a lock can be
 * acquired from both thread context and IRQ/IPI context.
 */
typedef struct {
    volatile uint32_t next_ticket;
    volatile uint32_t now_serving;
} spinlock_t;

#define SPINLOCK_INIT { 0u, 0u }

static inline void spin_lock_init(spinlock_t* lock) {
    lock->next_ticket = 0u;
    lock->now_serving = 0u;
}

static inline uint64_t irq_save_disable(void) {
    uint64_t rflags = 0;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags) : : "memory");
    return rflags;
}

static inline void irq_restore(uint64_t rflags) {
    if (rflags & 0x200ULL) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

static inline void spin_lock(spinlock_t* lock) {
    uint32_t ticket = __atomic_fetch_add(&lock->next_ticket, 1u, __ATOMIC_RELAXED);
    while (__atomic_load_n(&lock->now_serving, __ATOMIC_ACQUIRE) != ticket) {
        __asm__ volatile ("pause" : : : "memory");
    }
}

static inline bool spin_trylock(spinlock_t* lock) {
    uint32_t serving = __atomic_load_n(&lock->now_serving, __ATOMIC_RELAXED);
    uint32_t expected = serving;
    if (__atomic_compare_exchange_n(&lock->next_ticket, &expected, serving + 1u,
                                    false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return true;
    }
    return false;
}

static inline void spin_unlock(spinlock_t* lock) {
    uint32_t next = lock->now_serving + 1u;
    __atomic_store_n(&lock->now_serving, next, __ATOMIC_RELEASE);
}

static inline uint64_t spin_lock_irqsave(spinlock_t* lock) {
    uint64_t flags = irq_save_disable();
    spin_lock(lock);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t* lock, uint64_t flags) {
    spin_unlock(lock);
    irq_restore(flags);
}

/*
 * Sleepable Kernel Mutex for slower paths (VFS, storage, WM, audio).
 * Yields the CPU while contended once the scheduler is active.
 */
typedef struct {
    spinlock_t guard;
    volatile int locked;
    void* owner;
} kmutex_t;

#define KMUTEX_INIT { SPINLOCK_INIT, 0, NULL }

void kmutex_init(kmutex_t* mtx);
void kmutex_lock(kmutex_t* mtx);
bool kmutex_trylock(kmutex_t* mtx);
void kmutex_unlock(kmutex_t* mtx);

#endif /* _SPINLOCK_H */
