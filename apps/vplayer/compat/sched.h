#ifndef _VPLAYER_COMPAT_SCHED_H
#define _VPLAYER_COMPAT_SCHED_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2

#define CPU_SETSIZE 128

typedef struct {
    uint64_t bits[2];
} cpu_set_t;

static inline void CPU_ZERO(cpu_set_t* set) {
    set->bits[0] = 0;
    set->bits[1] = 0;
}

static inline void CPU_SET(int cpu, cpu_set_t* set) {
    if (cpu >= 0 && cpu < 64) set->bits[0] |= (1ULL << cpu);
    else if (cpu >= 64 && cpu < 128) set->bits[1] |= (1ULL << (cpu - 64));
}

static inline int CPU_ISSET(int cpu, const cpu_set_t* set) {
    if (cpu >= 0 && cpu < 64) return (int)((set->bits[0] >> cpu) & 1ULL);
    if (cpu >= 64 && cpu < 128) return (int)((set->bits[1] >> (cpu - 64)) & 1ULL);
    return 0;
}

static inline int CPU_COUNT(const cpu_set_t* set) {
    return __builtin_popcountll(set->bits[0]) + __builtin_popcountll(set->bits[1]);
}

int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t* mask);
int sched_yield(void);

#ifdef __cplusplus
}
#endif

#endif

