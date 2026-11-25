#include "spinlock.h"
#include "scheduler.h"
#include <stddef.h>

void kmutex_init(kmutex_t* mtx) {
    if (!mtx) return;
    spin_lock_init(&mtx->guard);
    mtx->locked = 0;
    mtx->owner = NULL;
}

bool kmutex_trylock(kmutex_t* mtx) {
    if (!mtx) return false;
    uint64_t flags = spin_lock_irqsave(&mtx->guard);
    bool acquired = false;
    if (!mtx->locked) {
        mtx->locked = 1;
        mtx->owner = scheduler_current_thread();
        acquired = true;
    }
    spin_unlock_irqrestore(&mtx->guard, flags);
    return acquired;
}

void kmutex_lock(kmutex_t* mtx) {
    if (!mtx) return;
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&mtx->guard);
        if (!mtx->locked) {
            mtx->locked = 1;
            mtx->owner = scheduler_current_thread();
            spin_unlock_irqrestore(&mtx->guard, flags);
            return;
        }
        spin_unlock_irqrestore(&mtx->guard, flags);

        /* Yield if interrupts are enabled and scheduler is running */
        uint64_t rflags = 0;
        __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
        if ((rflags & 0x200ULL) && scheduler_is_ready() && scheduler_current_thread()) {
            scheduler_mark_current_ready();
            scheduler_yield_kernel();
        } else {
            __asm__ volatile ("pause" : : : "memory");
        }
    }
}

void kmutex_unlock(kmutex_t* mtx) {
    if (!mtx) return;
    uint64_t flags = spin_lock_irqsave(&mtx->guard);
    mtx->owner = NULL;
    mtx->locked = 0;
    spin_unlock_irqrestore(&mtx->guard, flags);
}
