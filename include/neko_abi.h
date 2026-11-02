#ifndef _NEKO_ABI_H
#define _NEKO_ABI_H

#include <stdint.h>

/*
 * Shared kernel/user ABI. Keep this header freestanding and do not add
 * kernel-only types here because userland/init.elf includes it as well.
 */

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

#define NEKO_EPERM                 1
#define NEKO_EFAULT                14
#define NEKO_EAGAIN                11
#define NEKO_ENOMEM                12
#define NEKO_ECHILD                10
#define NEKO_EINVAL                22
#define NEKO_ENOENT                2
#define NEKO_ENOSPC                28

#define NEKO_USER_LIMIT            0x0000800000000000ULL
#define NEKO_SHM_BASE              0x0000600000000000ULL
#define NEKO_SHM_STRIDE            0x0000000000100000ULL
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
