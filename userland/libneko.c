#include "neko_abi.h"

static long neko_syscall6(long number, long arg0, long arg1, long arg2,
                          long arg3, long arg4, long arg5) {
    register long r10 __asm__("r10") = arg3;
    register long r8 __asm__("r8") = arg4;
    register long r9 __asm__("r9") = arg5;
    long result = number;

    __asm__ volatile (
        "syscall"
        : "+a"(result)
        : "D"(arg0), "S"(arg1), "d"(arg2), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return result;
}

static unsigned long local_strlen(const char* text) {
    unsigned long length = 0;
    while (text && text[length]) length++;
    return length;
}

static int local_strcmp(const char* left, const char* right) {
    while (*left && *left == *right) {
        left++;
        right++;
    }
    return (unsigned char)*left - (unsigned char)*right;
}

long neko_write(const char* text) {
    if (!text) return -NEKO_EINVAL;
    return neko_syscall6(NEKO_SYSCALL_WRITE, 1, (long)text,
                         (long)local_strlen(text), 0, 0, 0);
}

long neko_write_value(const char* label, long value) {
    char output[96];
    unsigned long cursor = 0;
    while (label && label[cursor] && cursor < 48) {
        output[cursor] = label[cursor];
        cursor++;
    }
    output[cursor++] = ':';
    output[cursor++] = ' ';
    if (value < 0) {
        output[cursor++] = '-';
        value = -value;
    }
    output[cursor++] = '0';
    output[cursor++] = 'x';

    char digits[16];
    unsigned long count = 0;
    unsigned long raw = (unsigned long)value;
    do {
        unsigned long digit = raw & 0xF;
        digits[count++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
        raw >>= 4;
    } while (raw && count < sizeof(digits));
    while (count > 0) {
        output[cursor++] = digits[--count];
    }
    output[cursor] = '\0';
    return neko_write(output);
}

long neko_getpid(void) {
    return neko_syscall6(NEKO_SYSCALL_GETPID, 0, 0, 0, 0, 0, 0);
}

long neko_yield(void) {
    return neko_syscall6(NEKO_SYSCALL_YIELD, 0, 0, 0, 0, 0, 0);
}

long neko_sleep(unsigned long ticks) {
    return neko_syscall6(NEKO_SYSCALL_SLEEP, (long)ticks, 0, 0, 0, 0, 0);
}

unsigned long neko_ticks(void) {
    return (unsigned long)neko_syscall6(NEKO_SYSCALL_GETTICKS,
                                        0, 0, 0, 0, 0, 0);
}

long neko_ipc_send(unsigned long queue_id, const neko_message_t* message) {
    return neko_syscall6(NEKO_SYSCALL_IPC_SEND, (long)queue_id,
                         (long)message, 0, 0, 0, 0);
}

long neko_ipc_recv(unsigned long queue_id, neko_message_t* message) {
    return neko_syscall6(NEKO_SYSCALL_IPC_RECV, (long)queue_id,
                         (long)message, 0, 0, 0, 0);
}

long neko_shm_attach(unsigned long shm_id) {
    return neko_syscall6(NEKO_SYSCALL_SHM_ATTACH, (long)shm_id,
                         0, 0, 0, 0, 0);
}

long neko_waitpid(unsigned long pid, int* status) {
    return neko_syscall6(NEKO_SYSCALL_WAITPID, (long)pid, (long)status,
                         0, 0, 0, 0);
}

long neko_nmi_test(void) {
    return neko_syscall6(NEKO_SYSCALL_NMI_TEST, 0, 0, 0, 0, 0, 0);
}

unsigned long neko_nmi_count(void) {
    return (unsigned long)neko_syscall6(NEKO_SYSCALL_NMI_COUNT,
                                        0, 0, 0, 0, 0, 0);
}

void neko_exit(int status) {
    neko_syscall6(NEKO_SYSCALL_EXIT, status, 0, 0, 0, 0, 0);
    for (;;) __asm__ volatile ("pause");
}

unsigned long neko_fpu_switch_test(unsigned long value) {
    unsigned long result = 0;
    /*
     * XMM0 remains live across a Ring 3 -> scheduler -> Ring 3 transition.
     * The other test process deliberately loads a different value.
     */
    __asm__ volatile (
        "movq %2, %%xmm0\n\t"
        "syscall\n\t"
        "movq %%xmm0, %0"
        : "=r"(result)
        : "a"((long)NEKO_SYSCALL_YIELD), "r"(value)
        : "rcx", "r11", "xmm0", "memory"
    );
    return result;
}

int user_strcmp(const char* left, const char* right) {
    return local_strcmp(left, right);
}
