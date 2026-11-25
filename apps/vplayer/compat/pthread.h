#ifndef _VPLAYER_COMPAT_PTHREAD_H
#define _VPLAYER_COMPAT_PTHREAD_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <sched.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <pxe/thread.h>

#define PTHREAD_SCOPE_SYSTEM  0
#define PTHREAD_SCOPE_PROCESS 1
#define PTHREAD_MUTEX_INITIALIZER { 0u }
#define PTHREAD_COND_INITIALIZER  { 0u }

typedef pxe_thread_t pthread_t;
typedef pxe_mutex_t  pthread_mutex_t;
typedef pxe_cond_t   pthread_cond_t;

typedef struct {
    int    policy;
    int    scope;
    size_t stack_size;
} pthread_attr_t;

typedef struct {
    int dummy;
} pthread_mutexattr_t;

typedef struct {
    int dummy;
} pthread_condattr_t;

int pthread_attr_init(pthread_attr_t* attr);
int pthread_attr_destroy(pthread_attr_t* attr);
int pthread_attr_setscope(pthread_attr_t* attr, int scope);
int pthread_attr_setschedpolicy(pthread_attr_t* attr, int policy);
int pthread_attr_setstacksize(pthread_attr_t* attr, size_t stacksize);

int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                   void* (*start_routine)(void*), void* arg);
int pthread_join(pthread_t thread, void** retval);
pthread_t pthread_self(void);

int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr);
int pthread_mutex_destroy(pthread_mutex_t* mutex);
int pthread_mutex_lock(pthread_mutex_t* mutex);
int pthread_mutex_trylock(pthread_mutex_t* mutex);
int pthread_mutex_unlock(pthread_mutex_t* mutex);

int pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr);
int pthread_cond_destroy(pthread_cond_t* cond);
int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex);
int pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex,
                           const struct timespec* abstime);
int pthread_cond_signal(pthread_cond_t* cond);
int pthread_cond_broadcast(pthread_cond_t* cond);

#ifdef __cplusplus
}
#endif

#endif
