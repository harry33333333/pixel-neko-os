#include "neko_abi.h"

long neko_write(const char* text);
long neko_getpid(void);
long neko_yield(void);
long neko_sleep(unsigned long ticks);
unsigned long neko_ticks(void);
long neko_ipc_send(unsigned long queue_id, const neko_message_t* message);
long neko_ipc_recv(unsigned long queue_id, neko_message_t* message);
long neko_shm_attach(unsigned long shm_id);
long neko_waitpid(unsigned long pid, int* status);
void neko_exit(int status);
unsigned long neko_fpu_switch_test(unsigned long value);
int user_strcmp(const char* left, const char* right);

static int fpu_arithmetic_test(void) {
    volatile double value = 3.25;
    double result = value * 2.0;
    return result == 6.5;
}

static int init_main(void) {
    long pid = neko_getpid();
    unsigned long fpu_magic = 0x1122334455667788UL;
    if (neko_fpu_switch_test(fpu_magic) != fpu_magic) {
        neko_write("P3 FPU context test FAILED in init process");
        return 5;
    }
    if (!fpu_arithmetic_test()) {
        neko_write("P3 SSE2 arithmetic test FAILED in init process");
        return 6;
    }

    long virt = neko_shm_attach(NEKO_TEST_SHM_ID);
    if (virt < 0) {
        neko_write("P3 shared-memory attach FAILED in init process");
        return 7;
    }

    volatile unsigned long* shared = (volatile unsigned long*)virt;
    shared[0] = 0x504958454c4e454bUL;

    neko_message_t ping;
    for (unsigned long i = 0; i < sizeof(ping) / sizeof(unsigned long); i++) {
        ((unsigned long*)&ping)[i] = 0;
    }
    ping.type = NEKO_TEST_PING;
    ping.arg0 = fpu_magic;
    ping.text[0] = 'P';
    ping.text[1] = 'I';
    ping.text[2] = 'N';
    ping.text[3] = 'G';

    if (neko_ipc_send(NEKO_TEST_QUEUE_ID, &ping) < 0) {
        neko_write("P3 IPC send FAILED in init process");
        return 8;
    }

    neko_message_t reply;
    long reply_result = -1;
    for (int i = 0; i < 200; i++) {
        reply_result = neko_ipc_recv(NEKO_TEST_RESPONSE_QUEUE_ID, &reply);
        if (reply_result == 0 && reply.type == NEKO_TEST_PONG) break;
        neko_sleep(1);
    }

    if (reply_result != 0) {
        neko_write("P3 IPC response receive FAILED");
        return 9;
    }
    if (reply.type != NEKO_TEST_PONG) {
        neko_write("P3 IPC response type FAILED");
        return 9;
    }
    if (shared[1] != 0x504f4e475245504cUL) {
        neko_write("P3 shared-memory visibility FAILED");
        return 9;
    }
    neko_write("P3 IPC/shared-memory round trip PASS");

    int child_status = -1;
    for (int i = 0; i < 200; i++) {
        if (neko_waitpid((unsigned long)reply.sender_pid, &child_status) ==
            reply.sender_pid) {
            break;
        }
        neko_sleep(1);
    }

    neko_write("P3 PASS: Ring 3, SYSCALL, FPU context, IPC queue and shared memory");
    return child_status == 0 ? 0 : 10;
}

static int echo_main(void) {
    unsigned long fpu_magic = 0x8877665544332211UL;
    if (neko_fpu_switch_test(fpu_magic) != fpu_magic) {
        neko_write("P3 FPU context test FAILED in echo process");
        return 11;
    }

    long virt = neko_shm_attach(NEKO_TEST_SHM_ID);
    if (virt < 0) {
        neko_write("P3 shared-memory attach FAILED in echo process");
        return 12;
    }

    volatile unsigned long* shared = (volatile unsigned long*)virt;
    neko_message_t ping;
    if (neko_ipc_recv(NEKO_TEST_QUEUE_ID, &ping) < 0) {
        return 13;
    }
    if (ping.type != NEKO_TEST_PING ||
        shared[0] != 0x504958454c4e454bUL) {
        neko_write("P3 echo received an invalid IPC message");
        return 14;
    }

    shared[1] = 0x504f4e475245504cUL;

    neko_message_t pong;
    for (unsigned long i = 0; i < sizeof(pong) / sizeof(unsigned long); i++) {
        ((unsigned long*)&pong)[i] = 0;
    }
    pong.type = NEKO_TEST_PONG;
    pong.arg0 = ping.arg0;
    pong.text[0] = 'P';
    pong.text[1] = 'O';
    pong.text[2] = 'N';
    pong.text[3] = 'G';

    if (neko_ipc_send(NEKO_TEST_RESPONSE_QUEUE_ID, &pong) < 0) {
        return 15;
    }

    neko_write("P3 echo process received PING and sent PONG");
    return 0;
}

int user_main(int argc, char** argv) {
    (void)argc;
    if (argv[1] && user_strcmp(argv[1], "echo") == 0) {
        return echo_main();
    }

    neko_write("P3 init entered Ring 3 through an ELF PT_LOAD image");
    return init_main();
}
