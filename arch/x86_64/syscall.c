#include "syscall.h"
#include "scheduler.h"
#include "process.h"
#include "percpu.h"
#include "ipc.h"
#include "cpu.h"
#include "gdt.h"
#include "log.h"
#include "string.h"
#include "neko_abi.h"

static char g_user_write_buffer[192];

void syscall_init(void) {
    per_cpu_init(&per_cpu_block);

    uint64_t efer = rdmsr(MSR_IA32_EFER);
    efer |= EFER_SCE | EFER_NXE;
    wrmsr(MSR_IA32_EFER, efer);

    /*
     * STAR[47:32] = kernel CS (0x08); STAR[63:48] = user base (0x10).
     * SYSRET therefore loads CS=0x20, SS=0x18.
     */
    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)GDT_KERNEL_CODE_SEL << 32);
    wrmsr(MSR_IA32_STAR, star);
    wrmsr(MSR_IA32_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_IA32_FMASK, (1ULL << 9) | (1ULL << 10)); // IF=0, DF=0

    log_printf("[SYSCALL] SYSCALL/SYSRET active: STAR=0x%016llx LSTAR=%p FMASK=0x600\n",
               (unsigned long long)star, (void*)syscall_entry);
    log_printf("[SYSCALL] GS_BASE per-CPU block @ %p, paranoid NMI uses IST2.\n",
               (void*)&per_cpu_block);
}

uint64_t syscall_get_count(void) {
    return per_cpu_block.syscall_count;
}

uint64_t nmi_get_count(void) {
    return per_cpu_block.nmi_count;
}

void syscall_mark_reschedule(void) {
    per_cpu_request_reschedule();
}

bool syscall_reschedule_requested(void) {
    return per_cpu_reschedule_requested();
}

trap_frame_t* syscall_reschedule(trap_frame_t* frame) {
    return scheduler_reschedule_from_syscall(frame);
}

trap_frame_t* syscall_bad_return(trap_frame_t* frame) {
    log_printf("[SYSCALL] Rejected non-canonical user return RIP=0x%016llx; terminating PID %u.\n",
               (unsigned long long)(frame ? frame->rip : 0), process_current_pid());
    process_exit_current(-1);
    return scheduler_reschedule_from_syscall(frame);
}

static int64_t syscall_write(trap_frame_t* frame) {
    uint64_t user_ptr = frame->rsi;
    size_t length = (size_t)frame->rdx;
    if (length == 0) return 0;
    if (length >= sizeof(g_user_write_buffer)) {
        length = sizeof(g_user_write_buffer) - 1;
    }
    if (!process_current() ||
        !process_copy_from_user(process_current(), g_user_write_buffer,
                                user_ptr, length)) {
        return -NEKO_EFAULT;
    }

    g_user_write_buffer[length] = '\0';
    log_printf("[RING3:%u] %s\n", process_current_pid(), g_user_write_buffer);
    return (int64_t)length;
}

static int64_t syscall_waitpid(trap_frame_t* frame) {
    process_t* parent = process_current();
    if (!parent) return -NEKO_ECHILD;

    uint32_t pid = (uint32_t)frame->rdi;
    process_t* child = process_find_by_pid(pid);
    if (!child || child->parent != parent) return -NEKO_ECHILD;
    if (child->state != PROCESS_ZOMBIE) return -NEKO_EAGAIN;

    uint64_t status_dst = frame->rsi;
    if (status_dst && !process_copy_to_user(parent, status_dst,
                                            &child->exit_status,
                                            sizeof(child->exit_status))) {
        return -NEKO_EFAULT;
    }
    return pid;
}

int64_t syscall_dispatcher(trap_frame_t* frame) {
    if (!frame) return -NEKO_EINVAL;

    process_t* process = process_current();
    thread_t* thread = scheduler_current_thread();
    if (!process || !thread || process->state == PROCESS_ZOMBIE) {
        return -NEKO_EPERM;
    }

    int64_t result = 0;
    switch (frame->rax) {
        case NEKO_SYSCALL_EXIT:
            process_exit_current((int)frame->rdi);
            result = 0;
            break;

        case NEKO_SYSCALL_WRITE:
            result = syscall_write(frame);
            break;

        case NEKO_SYSCALL_GETPID:
            result = process->pid;
            break;

        case NEKO_SYSCALL_YIELD:
            scheduler_mark_current_ready();
            syscall_mark_reschedule();
            result = 0;
            break;

        case NEKO_SYSCALL_SLEEP: {
            uint64_t ticks = frame->rdi;
            if (ticks == 0) {
                result = 0;
                break;
            }
            frame->rip += 2; // SYSCALL is a two-byte instruction.
            scheduler_block_current(scheduler_get_ticks() + ticks);
            result = 0;
            break;
        }

        case NEKO_SYSCALL_GETTICKS:
            result = (int64_t)scheduler_get_ticks();
            break;

        case NEKO_SYSCALL_IPC_SEND: {
            neko_message_t message;
            if (!process_copy_from_user(process, &message, frame->rsi,
                                        sizeof(message))) {
                result = -NEKO_EFAULT;
                break;
            }
            result = ipc_send(frame->rdi, &message, process->pid);
            break;
        }

        case NEKO_SYSCALL_IPC_RECV: {
            result = ipc_recv_block(process, thread, frame->rdi, frame->rsi);
            if (thread->state == THREAD_BLOCKED) {
                frame->rip += 2;
                result = 0;
            }
            break;
        }

        case NEKO_SYSCALL_SHM_ATTACH:
            result = ipc_shm_attach(process, frame->rdi);
            break;

        case NEKO_SYSCALL_WAITPID:
            result = syscall_waitpid(frame);
            break;

        default:
            result = -NEKO_EINVAL;
            break;
    }

    frame->rax = (uint64_t)result;
    return result;
}

void c_nmi_handler(trap_frame_t* frame) {
    per_cpu_block.nmi_count++;

    uint64_t kernel_gs = rdmsr(MSR_GS_BASE);
    if (kernel_gs != (uint64_t)&per_cpu_block) {
        log_printf("[NMI CRITICAL] GS_BASE identity mismatch: 0x%016llx != %p\n",
                   (unsigned long long)kernel_gs, (void*)&per_cpu_block);
    }

    if (per_cpu_block.nmi_count <= 4) {
        log_printf("[NMI] Paranoid entry #%llu RIP=0x%016llx CS=0x%04llx GS=%s\n",
                   (unsigned long long)per_cpu_block.nmi_count,
                   (unsigned long long)(frame ? frame->rip : 0),
                   (unsigned long long)(frame ? frame->cs : 0),
                   kernel_gs == (uint64_t)&per_cpu_block ? "kernel" : "invalid");
    }
}
