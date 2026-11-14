#ifndef _NEKO_ABI_H
#define _NEKO_ABI_H

#include <stdint.h>

/*
 * Shared kernel/user ABI. Keep this header freestanding and do not add
 * kernel-only types here because userland/init.elf and libpxe_r3 include it.
 */

/* Legacy P3 Core Syscalls (0..11) */
#define NEKO_SYSCALL_EXIT          0
#define NEKO_SYSCALL_WRITE         1
#define NEKO_SYSCALL_GETPID        2
#define NEKO_SYSCALL_YIELD         3
#define NEKO_SYSCALL_SLEEP         4
#define NEKO_SYSCALL_GETTICKS      5
#define NEKO_SYSCALL_IPC_SEND      6
#define NEKO_SYSCALL_IPC_RECV      7
#define NEKO_SYSCALL_SHM_ATTACH    8
#define NEKO_SYSCALL_WAITPID       9
#define NEKO_SYSCALL_NMI_TEST      10
#define NEKO_SYSCALL_NMI_COUNT     11

/* PXE Standard Runtime Syscalls (20+) */
#define NEKO_SYSCALL_SBRK          20
#define NEKO_SYSCALL_SPAWN         21
#define NEKO_SYSCALL_GET_RTC       22
#define NEKO_SYSCALL_GET_SYSINFO   23
#define NEKO_SYSCALL_SYS_CONFIG    24
#define NEKO_SYSCALL_GET_DISP_CAPS 25

/* PXE GUI & Compositor Syscalls (30..39) */
#define NEKO_SYSCALL_WIN_CREATE    30
#define NEKO_SYSCALL_WIN_DESTROY   31
#define NEKO_SYSCALL_WIN_SET_TITLE 32
#define NEKO_SYSCALL_WIN_SET_MENU  33
#define NEKO_SYSCALL_WIN_PRESENT   34
#define NEKO_SYSCALL_WIN_POLL_EV   35
#define NEKO_SYSCALL_CLIPBOARD     36
#define NEKO_SYSCALL_NOTIFY_BANNER 37
#define NEKO_SYSCALL_BEEP          38
#define NEKO_SYSCALL_WIN_GET_SURF  39

/* PXE VFS Syscalls (40..54) */
#define NEKO_SYSCALL_FS_OPEN       40
#define NEKO_SYSCALL_FS_READ       41
#define NEKO_SYSCALL_FS_SEEK       42
#define NEKO_SYSCALL_FS_CLOSE      43
#define NEKO_SYSCALL_FS_STAT       44
#define NEKO_SYSCALL_FS_READ_FILE  45
#define NEKO_SYSCALL_FS_WRITE_FILE 46
#define NEKO_SYSCALL_FS_LISTDIR    47
#define NEKO_SYSCALL_FS_MKDIR      48
#define NEKO_SYSCALL_FS_UNLINK     49
#define NEKO_SYSCALL_FS_RENAME     50
#define NEKO_SYSCALL_FS_GET_DRIVES 51
#define NEKO_SYSCALL_FS_EJECT      52
#define NEKO_SYSCALL_FS_CREATE     53
#define NEKO_SYSCALL_FS_WRITE      54

/* PXE Audio Syscall (60) */
#define NEKO_SYSCALL_AUDIO_CMD     60

#define NEKO_EPERM                 1
#define NEKO_ENOENT                2
#define NEKO_ECHILD                10
#define NEKO_EAGAIN                11
#define NEKO_ENOMEM                12
#define NEKO_EFAULT                14
#define NEKO_EINVAL                22
#define NEKO_ENOSPC                28

#define NEKO_USER_LIMIT            0x0000800000000000ULL
#define NEKO_SHM_BASE              0x0000600000000000ULL
#define NEKO_SHM_STRIDE            0x0000000004000000ULL /* 64 MB stride per mapped surface */
#define NEKO_TEST_QUEUE_ID         1ULL
#define NEKO_TEST_RESPONSE_QUEUE_ID 2ULL
#define NEKO_TEST_SHM_ID           1ULL
#define NEKO_TEST_PING             0x4e454b4f50494e47ULL
#define NEKO_TEST_PONG             0x4e454b4f504f4e47ULL

typedef struct {
    uint64_t type;
    uint64_t sender_pid;
    uint64_t arg0;
    uint64_t arg1;
    char text[32];
} neko_message_t;

#endif /* _NEKO_ABI_H */
