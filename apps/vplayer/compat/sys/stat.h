#ifndef _VPLAYER_COMPAT_SYS_STAT_H
#define _VPLAYER_COMPAT_SYS_STAT_H

#include <sys/types.h>

struct stat {
    uint64_t st_size;
    mode_t   st_mode;
};

#endif

