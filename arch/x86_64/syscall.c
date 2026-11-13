#include "syscall.h"
#include "scheduler.h"
#include "process.h"
#include "percpu.h"
#include "ipc.h"
#include "apic.h"
#include "cpu.h"
#include "gdt.h"
#include "log.h"
#include "string.h"
#include "neko_abi.h"
#include "pxe.h"
#include "wm.h"
#include "speaker.h"
#include "heap.h"

static char g_user_write_buffer[192];

void syscall_init(void) {
    per_cpu_init(&per_cpu_block);

    uint64_t efer = rdmsr(MSR_IA32_EFER);
    efer |= EFER_SCE | EFER_NXE;
    wrmsr(MSR_IA32_EFER, efer);

    /*
     * STAR[47:32] = kernel CS (0x08); STAR[63:48] = user base (0x10).
     * SYSRET applies RPL 3, loading CS=0x23 and SS=0x1b.
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
    log_printf("[SYSCALL] Rejected invalid return frame (RIP=0x%016llx RSP=0x%016llx RFLAGS=0x%08llx CS=0x%02llx SS=0x%02llx); terminating PID %u.\n",
               (unsigned long long)(frame ? frame->rip : 0),
               (unsigned long long)(frame ? frame->rsp : 0),
               (unsigned long long)(frame ? frame->rflags : 0),
               (unsigned long long)(frame ? frame->cs : 0),
               (unsigned long long)(frame ? frame->ss : 0),
               process_current_pid());
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
    child->waited = true;
    return pid;
}

static bool copy_user_string(process_t* proc, uint64_t user_ptr, char* dst, size_t cap) {
    if (!proc || !user_ptr || !dst || cap == 0) return false;
    for (size_t i = 0; i < cap; i++) {
        char ch = 0;
        if (!process_copy_from_user(proc, &ch, user_ptr + i, 1)) {
            dst[0] = '\0';
            return false;
        }
        dst[i] = ch;
        if (ch == '\0') return true;
    }
    dst[cap - 1] = '\0';
    return true;
}

static int64_t syscall_spawn(process_t* process, trap_frame_t* frame) {
    char kpath[128];
    if (!copy_user_string(process, frame->rdi, kpath, sizeof(kpath))) {
        return -NEKO_EFAULT;
    }

    int argc = (int)(int64_t)frame->rsi;
    if (argc < 0) argc = 0;
    if (argc > 16) argc = 16;

    const char* kargv[16];
    char* karg_buf = NULL;
    uint64_t user_argv[16];

    if (argc > 0 && frame->rdx != 0) {
        if (!process_copy_from_user(process, user_argv, frame->rdx,
                                    (size_t)argc * sizeof(uint64_t))) {
            return -NEKO_EFAULT;
        }
        karg_buf = (char*)kmalloc((size_t)argc * 128U);
        if (!karg_buf) return -NEKO_ENOMEM;
        for (int i = 0; i < argc; i++) {
            char* slot = karg_buf + (size_t)i * 128U;
            if (!user_argv[i]) {
                slot[0] = '\0';
            } else if (!copy_user_string(process, user_argv[i], slot, 128U)) {
                kfree(karg_buf);
                return -NEKO_EFAULT;
            }
            kargv[i] = slot;
        }
    } else {
        argc = 0;
    }

    int64_t rc = pxe_spawn_file(kpath, argc, argc > 0 ? kargv : NULL, process, false);
    if (karg_buf) kfree(karg_buf);
    return rc;
}

static int64_t syscall_fs_read(process_t* process, trap_frame_t* frame) {
    int fd = (int)frame->rdi;
    uint64_t user_dst = frame->rsi;
    size_t count = (size_t)frame->rdx;
    if (count == 0) return 0;
    if (count > 8U * 1024U * 1024U) count = 8U * 1024U * 1024U;
    if (!user_dst || !process_user_range_ok(process, user_dst, count, true)) {
        return -NEKO_EFAULT;
    }

    size_t chunk_cap = (count > 65536U) ? 65536U : count;
    uint8_t* kbuf = (uint8_t*)kmalloc(chunk_cap);
    if (!kbuf) return -NEKO_ENOMEM;

    size_t total = 0;
    while (total < count) {
        size_t want = count - total;
        if (want > chunk_cap) want = chunk_cap;
        int n = pxe_kern_fs_read(process, fd, kbuf, want);
        if (n < 0) {
            kfree(kbuf);
            return total > 0 ? (int64_t)total : n;
        }
        if (n == 0) break;
        if (!process_copy_to_user(process, user_dst + total, kbuf, (size_t)n)) {
            kfree(kbuf);
            return -NEKO_EFAULT;
        }
        total += (size_t)n;
        if ((size_t)n < want) break;
    }

    kfree(kbuf);
    return (int64_t)total;
}

static int64_t syscall_audio_cmd(process_t* process, trap_frame_t* frame) {
    int cmd = (int)frame->rdi;
    int arg = (int)frame->rsi;
    uint64_t user_path = frame->rdx;
    uint64_t user_io = frame->r10;

    if (cmd == PXE_AUD_GET_STATUS) {
        if (!user_io) return -NEKO_EINVAL;
        pxe_audio_status_t st;
        int rc = pxe_kern_audio_cmd(PXE_AUD_GET_STATUS, 0, NULL, &st);
        if (rc < 0) return rc;
        if (!process_copy_to_user(process, user_io, &st, sizeof(st))) {
            return -NEKO_EFAULT;
        }
        return rc;
    }

    if (cmd == PXE_AUD_FEED_PCM) {
        if (!user_io) return -NEKO_EINVAL;
        pxe_pcm_chunk_t uc;
        if (!process_copy_from_user(process, &uc, user_io, sizeof(uc))) {
            return -NEKO_EFAULT;
        }
        if (!uc.samples || uc.sample_count == 0 ||
            uc.src_channels < 1 || uc.src_channels > 8 ||
            (uc.src_bits != 8 && uc.src_bits != 16 && uc.src_bits != 32)) {
            return -NEKO_EINVAL;
        }
        uint64_t bytes = (uint64_t)uc.sample_count * (uint64_t)uc.src_channels *
                         (uint64_t)(uc.src_bits / 8);
        if (bytes == 0 || bytes > 262144ULL) return -NEKO_EINVAL;

        void* ksamples = kmalloc((size_t)bytes);
        if (!ksamples) return -NEKO_ENOMEM;
        if (!process_copy_from_user(process, ksamples, (uint64_t)(uintptr_t)uc.samples,
                                    (size_t)bytes)) {
            kfree(ksamples);
            return -NEKO_EFAULT;
        }
        pxe_pcm_chunk_t kc = uc;
        kc.samples = ksamples;
        int rc = pxe_kern_audio_cmd(PXE_AUD_FEED_PCM, arg, NULL, &kc);
        kfree(ksamples);
        return rc;
    }

    if (cmd == PXE_AUD_PLAY_FILE) {
        char kpath[128];
        if (!copy_user_string(process, user_path, kpath, sizeof(kpath))) {
            return -NEKO_EFAULT;
        }
        return pxe_kern_audio_cmd(PXE_AUD_PLAY_FILE, arg, kpath, NULL);
    }

    return pxe_kern_audio_cmd(cmd, arg, NULL, NULL);
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
            // Resume at the instruction after SYSCALL; user wrappers execute
            // their own RET immediately after the syscall instruction.
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

        case NEKO_SYSCALL_NMI_TEST:
            per_cpu_nmi_test_pending = 1;
            result = 0;
            break;

        case NEKO_SYSCALL_NMI_COUNT:
            result = (int64_t)nmi_get_count();
            break;

        case NEKO_SYSCALL_SBRK:
            result = process_sbrk(process, (int64_t)frame->rdi);
            break;

        case NEKO_SYSCALL_SPAWN:
            result = syscall_spawn(process, frame);
            break;

        case NEKO_SYSCALL_GET_RTC: {
            if (!frame->rdi) { result = -NEKO_EINVAL; break; }
            pxe_rtc_time_t rtc;
            result = pxe_kern_get_rtc(&rtc);
            if (result >= 0 && !process_copy_to_user(process, frame->rdi, &rtc, sizeof(rtc))) {
                result = -NEKO_EFAULT;
            }
            break;
        }

        case NEKO_SYSCALL_GET_SYSINFO: {
            if (!frame->rdi) { result = -NEKO_EINVAL; break; }
            pxe_basic_sysinfo_t si;
            result = pxe_kern_get_basic_sysinfo(&si);
            if (result >= 0 && !process_copy_to_user(process, frame->rdi, &si, sizeof(si))) {
                result = -NEKO_EFAULT;
            }
            break;
        }

        case NEKO_SYSCALL_GET_DISP_CAPS: {
            if (!frame->rdi) { result = -NEKO_EINVAL; break; }
            pxe_display_caps_t caps;
            result = pxe_kern_get_display_caps(&caps);
            if (result >= 0 && !process_copy_to_user(process, frame->rdi, &caps, sizeof(caps))) {
                result = -NEKO_EFAULT;
            }
            break;
        }

        case NEKO_SYSCALL_SYS_CONFIG: {
            char str_arg[128];
            const char* kstr = NULL;
            if (frame->r10 != 0) {
                if (!copy_user_string(process, frame->r10, str_arg, sizeof(str_arg))) {
                    result = -NEKO_EFAULT;
                    break;
                }
                kstr = str_arg;
            }
            result = pxe_kern_sys_config(process, (int)frame->rdi, (int)frame->rsi,
                                         (int)frame->rdx, kstr);
            break;
        }

        case NEKO_SYSCALL_WIN_CREATE: {
            if (!frame->rdi || !frame->rsi) { result = -NEKO_EINVAL; break; }
            pxe_win_desc_t desc;
            pxe_win_surface_t surf;
            if (!process_copy_from_user(process, &desc, frame->rdi, sizeof(desc))) {
                result = -NEKO_EFAULT;
                break;
            }
            int wid = wm_pxe_win_create(process, &desc, &surf);
            if (wid >= 0 && !process_copy_to_user(process, frame->rsi, &surf, sizeof(surf))) {
                wm_pxe_win_destroy(process->pid, wid);
                result = -NEKO_EFAULT;
                break;
            }
            result = wid;
            break;
        }

        case NEKO_SYSCALL_WIN_DESTROY:
            result = wm_pxe_win_destroy(process->pid, (int)frame->rdi);
            break;

        case NEKO_SYSCALL_WIN_SET_TITLE: {
            char title[48];
            if (!copy_user_string(process, frame->rsi, title, sizeof(title))) {
                result = -NEKO_EFAULT;
                break;
            }
            result = wm_pxe_win_set_title(process->pid, (int)frame->rdi, title);
            break;
        }

        case NEKO_SYSCALL_WIN_SET_MENU: {
            if (frame->rsi == 0) {
                result = wm_pxe_win_set_menu(process->pid, (int)frame->rdi, NULL);
                break;
            }
            pxe_menu_bar_t* kmenu = (pxe_menu_bar_t*)kmalloc(sizeof(pxe_menu_bar_t));
            if (!kmenu) { result = -NEKO_ENOMEM; break; }
            if (!process_copy_from_user(process, kmenu, frame->rsi, sizeof(pxe_menu_bar_t))) {
                kfree(kmenu);
                result = -NEKO_EFAULT;
                break;
            }
            result = wm_pxe_win_set_menu(process->pid, (int)frame->rdi, kmenu);
            kfree(kmenu);
            break;
        }

        case NEKO_SYSCALL_WIN_PRESENT:
            result = wm_pxe_win_present(process->pid, (int)frame->rdi,
                                        (int)frame->rsi, (int)frame->rdx,
                                        (int)frame->r10, (int)frame->r8);
            break;

        case NEKO_SYSCALL_WIN_POLL_EV: {
            if (!frame->rsi ||
                !process_user_range_ok(process, frame->rsi, sizeof(pxe_event_t), true)) {
                result = -NEKO_EFAULT;
                break;
            }
            pxe_event_t ev;
            int wid = (int)frame->rdi;
            int block = (int)frame->rdx;
            int rc = wm_pxe_win_poll_event(process->pid, wid, &ev);
            if (rc > 0) {
                if (!process_copy_to_user(process, frame->rsi, &ev, sizeof(ev))) {
                    result = -NEKO_EFAULT;
                    break;
                }
            } else if (rc == 0 && block) {
                scheduler_block_on_window(wid, 5);
            }
            result = rc;
            break;
        }

        case NEKO_SYSCALL_CLIPBOARD: {
            int is_set = (int)frame->rdi;
            uint64_t ubuf = frame->rsi;
            size_t max_len = (size_t)frame->rdx;
            if (!ubuf) { result = -NEKO_EINVAL; break; }
            if (is_set) {
                char* kbuf = (char*)kmalloc(4096);
                if (!kbuf) { result = -NEKO_ENOMEM; break; }
                if (!copy_user_string(process, ubuf, kbuf, 4096)) {
                    kfree(kbuf);
                    result = -NEKO_EFAULT;
                    break;
                }
                result = pxe_kern_clipboard_op(1, kbuf, 4096);
                kfree(kbuf);
            } else {
                if (max_len == 0) { result = 0; break; }
                if (max_len > 4096U) max_len = 4096U;
                char* kbuf = (char*)kmalloc(max_len);
                if (!kbuf) { result = -NEKO_ENOMEM; break; }
                int rc = pxe_kern_clipboard_op(0, kbuf, max_len);
                if (rc >= 0) {
                    size_t cpy = (size_t)rc + 1U;
                    if (cpy > max_len) cpy = max_len;
                    if (!process_copy_to_user(process, ubuf, kbuf, cpy)) {
                        rc = -NEKO_EFAULT;
                    }
                }
                kfree(kbuf);
                result = rc;
            }
            break;
        }

        case NEKO_SYSCALL_NOTIFY_BANNER: {
            char msg[48];
            if (!copy_user_string(process, frame->rdi, msg, sizeof(msg))) {
                result = -NEKO_EFAULT;
                break;
            }
            wm_notify_banner(msg);
            result = 0;
            break;
        }

        case NEKO_SYSCALL_BEEP:
            speaker_beep();
            result = 0;
            break;

        case NEKO_SYSCALL_WIN_GET_SURF: {
            if (!frame->rsi) { result = -NEKO_EINVAL; break; }
            pxe_win_surface_t surf;
            int rc = wm_pxe_win_get_surface(process, (int)frame->rdi, &surf);
            if (rc >= 0 && !process_copy_to_user(process, frame->rsi, &surf, sizeof(surf))) {
                rc = -NEKO_EFAULT;
            }
            result = rc;
            break;
        }

        case NEKO_SYSCALL_FS_OPEN: {
            char kpath[128];
            if (!copy_user_string(process, frame->rdi, kpath, sizeof(kpath))) {
                result = -NEKO_EFAULT;
                break;
            }
            result = pxe_kern_fs_open(process, kpath);
            break;
        }

        case NEKO_SYSCALL_FS_READ:
            result = syscall_fs_read(process, frame);
            break;

        case NEKO_SYSCALL_FS_SEEK:
            result = pxe_kern_fs_seek(process, (int)frame->rdi, frame->rsi);
            break;

        case NEKO_SYSCALL_FS_CLOSE:
            result = pxe_kern_fs_close(process, (int)frame->rdi);
            break;

        case NEKO_SYSCALL_FS_STAT: {
            if (!frame->rsi) { result = -NEKO_EINVAL; break; }
            char kpath[128];
            pxe_dirent_t ent;
            if (!copy_user_string(process, frame->rdi, kpath, sizeof(kpath))) {
                result = -NEKO_EFAULT;
                break;
            }
            int rc = pxe_kern_fs_stat(kpath, &ent);
            if (rc >= 0 && !process_copy_to_user(process, frame->rsi, &ent, sizeof(ent))) {
                rc = -NEKO_EFAULT;
            }
            result = rc;
            break;
        }

        case NEKO_SYSCALL_FS_READ_FILE: {
            char kpath[128];
            uint64_t ubuf = frame->rsi;
            size_t max_size = (size_t)frame->rdx;
            if (!ubuf || max_size == 0) { result = -NEKO_EINVAL; break; }
            if (max_size > 8U * 1024U * 1024U) max_size = 8U * 1024U * 1024U;
            if (!copy_user_string(process, frame->rdi, kpath, sizeof(kpath))) {
                result = -NEKO_EFAULT;
                break;
            }
            pxe_dirent_t st;
            if (pxe_kern_fs_stat(kpath, &st) != 0 || st.is_dir) {
                result = -1;
                break;
            }
            if (st.size == 0) {
                result = 0;
                break;
            }
            size_t alloc_sz = (st.size < max_size) ? (size_t)st.size : max_size;
            if (!process_user_range_ok(process, ubuf, alloc_sz, true)) {
                result = -NEKO_EFAULT;
                break;
            }
            uint8_t* kbuf = (uint8_t*)kmalloc(alloc_sz);
            if (!kbuf) { result = -NEKO_ENOMEM; break; }
            int n = pxe_kern_fs_read_file(kpath, kbuf, alloc_sz);
            if (n > 0 && !process_copy_to_user(process, ubuf, kbuf, (size_t)n)) {
                n = -NEKO_EFAULT;
            }
            kfree(kbuf);
            result = n;
            break;
        }

        case NEKO_SYSCALL_FS_WRITE_FILE: {
            char kpath[128];
            uint64_t ubuf = frame->rsi;
            size_t sz = (size_t)frame->rdx;
            if (sz > 8U * 1024U * 1024U) { result = -NEKO_EINVAL; break; }
            if (!copy_user_string(process, frame->rdi, kpath, sizeof(kpath))) {
                result = -NEKO_EFAULT;
                break;
            }
            if (sz == 0) {
                result = pxe_kern_fs_write_file(kpath, NULL, 0);
                break;
            }
            if (!ubuf || !process_user_range_ok(process, ubuf, sz, false)) {
                result = -NEKO_EFAULT;
                break;
            }
            uint8_t* kbuf = (uint8_t*)kmalloc(sz);
            if (!kbuf) { result = -NEKO_ENOMEM; break; }
            if (!process_copy_from_user(process, kbuf, ubuf, sz)) {
                kfree(kbuf);
                result = -NEKO_EFAULT;
                break;
            }
            int rc = pxe_kern_fs_write_file(kpath, kbuf, sz);
            kfree(kbuf);
            result = rc;
            break;
        }

        case NEKO_SYSCALL_FS_LISTDIR: {
            char kpath[128];
            uint64_t uout = frame->rsi;
            int max_entries = (int)frame->rdx;
            if (!uout || max_entries <= 0) { result = -NEKO_EINVAL; break; }
            if (max_entries > 512) max_entries = 512;
            if (!copy_user_string(process, frame->rdi, kpath, sizeof(kpath))) {
                result = -NEKO_EFAULT;
                break;
            }
            size_t bytes = (size_t)max_entries * sizeof(pxe_dirent_t);
            if (!process_user_range_ok(process, uout, bytes, true)) {
                result = -NEKO_EFAULT;
                break;
            }
            pxe_dirent_t* kout = (pxe_dirent_t*)kmalloc(bytes);
            if (!kout) { result = -NEKO_ENOMEM; break; }
            int cnt = pxe_kern_fs_listdir(kpath, kout, max_entries);
            if (cnt > 0 &&
                !process_copy_to_user(process, uout, kout, (size_t)cnt * sizeof(pxe_dirent_t))) {
                cnt = -NEKO_EFAULT;
            }
            kfree(kout);
            result = cnt;
            break;
        }

        case NEKO_SYSCALL_FS_MKDIR: {
            char kpath[128];
            if (!copy_user_string(process, frame->rdi, kpath, sizeof(kpath))) {
                result = -NEKO_EFAULT;
                break;
            }
            result = pxe_kern_fs_mkdir(kpath);
            break;
        }

        case NEKO_SYSCALL_FS_UNLINK: {
            char kpath[128];
            if (!copy_user_string(process, frame->rdi, kpath, sizeof(kpath))) {
                result = -NEKO_EFAULT;
                break;
            }
            result = pxe_kern_fs_unlink(kpath);
            break;
        }

        case NEKO_SYSCALL_FS_RENAME: {
            char kold[128], knew[128];
            if (!copy_user_string(process, frame->rdi, kold, sizeof(kold)) ||
                !copy_user_string(process, frame->rsi, knew, sizeof(knew))) {
                result = -NEKO_EFAULT;
                break;
            }
            result = pxe_kern_fs_rename(kold, knew);
            break;
        }

        case NEKO_SYSCALL_FS_GET_DRIVES: {
            uint64_t uout = frame->rdi;
            int max_drives = (int)frame->rsi;
            if (!uout || max_drives <= 0) { result = -NEKO_EINVAL; break; }
            if (max_drives > 26) max_drives = 26;
            size_t bytes = (size_t)max_drives * sizeof(pxe_drive_info_t);
            if (!process_user_range_ok(process, uout, bytes, true)) {
                result = -NEKO_EFAULT;
                break;
            }
            pxe_drive_info_t* kout = (pxe_drive_info_t*)kmalloc(bytes);
            if (!kout) { result = -NEKO_ENOMEM; break; }
            int cnt = pxe_kern_fs_get_drives(kout, max_drives);
            if (cnt > 0 &&
                !process_copy_to_user(process, uout, kout, (size_t)cnt * sizeof(pxe_drive_info_t))) {
                cnt = -NEKO_EFAULT;
            }
            kfree(kout);
            result = cnt;
            break;
        }

        case NEKO_SYSCALL_FS_EJECT:
            result = pxe_kern_fs_eject((char)frame->rdi);
            break;

        case NEKO_SYSCALL_AUDIO_CMD:
            result = syscall_audio_cmd(process, frame);
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
