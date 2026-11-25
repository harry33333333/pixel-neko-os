#ifndef _VPLAYER_COMPAT_TIME_H
#define _VPLAYER_COMPAT_TIME_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

time_t     time(time_t* tloc);
int        clock_gettime(int clk_id, struct timespec* tp);
struct tm* localtime(const time_t* timer);
size_t     strftime(char* s, size_t maxsize, const char* fmt, const struct tm* tm);

#ifdef __cplusplus
}
#endif

#endif

