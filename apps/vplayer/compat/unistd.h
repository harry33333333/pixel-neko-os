#ifndef _VPLAYER_COMPAT_UNISTD_H
#define _VPLAYER_COMPAT_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define _SC_NPROCESSORS_ONLN 84

int  usleep(useconds_t usec);
unsigned int sleep(unsigned int seconds);
long sysconf(int name);

#ifdef __cplusplus
}
#endif

#endif

