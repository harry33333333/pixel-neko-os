#ifndef _VPLAYER_COMPAT_SEMAPHORE_H
#define _VPLAYER_COMPAT_SEMAPHORE_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <pxe/thread.h>

typedef pxe_sem_t sem_t;

int sem_init(sem_t* sem, int pshared, unsigned int value);
int sem_destroy(sem_t* sem);
int sem_wait(sem_t* sem);
int sem_trywait(sem_t* sem);
int sem_timedwait(sem_t* sem, const struct timespec* abs_timeout);
int sem_post(sem_t* sem);
int sem_getvalue(sem_t* sem, int* sval);

#ifdef __cplusplus
}
#endif

#endif
