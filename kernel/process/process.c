#include "process.h"
#include "scheduler.h"
#include "elf.h"
#include "pmm.h"
#include "heap.h"
#include "gdt.h"
#include "log.h"
#include "string.h"
#include "neko_abi.h"
#include "syscall.h"
#include "vfs.h"
#include "wm.h"

#define USER_HEAP_DEFAULT_BASE  0x0000000010000000ULL
#define USER_HEAP_DEFAULT_LIMIT 0x0000000050000000ULL

static process_t* g_process_list = NULL;
static process_t* g_current_process = NULL;
static uint32_t g_next_pid = 1;
static uint64_t g_next_tid = 1;

static void process_link(process_t* process) {
    if (!process) return;
    process->next = g_process_list;
    g_process_list = process;
}

process_t* process_head(void) {
    return g_process_list;
}

void process_unlink(process_t* target) {
    if (!target) return;
    for (process_t* q = g_process_list; q; q = q->next) {
        if (q->parent == target) {
            q->parent = NULL;
        }
    }

    process_t** pp = &g_process_list;
    while (*pp) {
        if (*pp == target) {
            *pp = target->next;
            target->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

void process_subsystem_init(void) {
    g_process_list = NULL;
    g_current_process = NULL;
    g_next_pid = 1;
    g_next_tid = 1;
}

process_t* process_create_empty(const char* name, process_t* parent) {
    process_t* process = (process_t*)kzalloc(sizeof(process_t));
    if (!process) return NULL;

    process->pml4 = vmm_create_address_space();
    if (!process->pml4) {
        kfree(process);
        return NULL;
    }
    if (!virt_to_phys_checked(process->pml4, &process->pml4_phys)) {
        vmm_destroy_address_space(process->pml4);
        kfree(process);
        return NULL;
    }

    process->state = PROCESS_READY;
    process->parent = parent;
    process->ppid = parent ? parent->pid : 0;
    process->exit_status = 0;
    process->exec_ring = 3;
    process->r0_slot = -1;
    process->heap_base = USER_HEAP_DEFAULT_BASE;
    process->heap_brk = USER_HEAP_DEFAULT_BASE;
    process->heap_limit = USER_HEAP_DEFAULT_LIMIT;
    process->next_shm_virt = NEKO_SHM_BASE + 4ULL * NEKO_SHM_STRIDE;
    process->mem_pages_used = 1; /* PML4 page */
    strncpy(process->name, name ? name : "user", PROCESS_MAX_NAME - 1);
    process->name[PROCESS_MAX_NAME - 1] = '\0';
    return process;
}

process_t* process_create_r0_empty(const char* name, process_t* parent) {
    process_t* process = (process_t*)kzalloc(sizeof(process_t));
    if (!process) return NULL;

    process->pml4 = vmm_get_kernel_pml4();
    if (!virt_to_phys_checked(process->pml4, &process->pml4_phys)) {
        kfree(process);
        return NULL;
    }

    process->state = PROCESS_READY;
    process->parent = parent;
    process->ppid = parent ? parent->pid : 0;
    process->exit_status = 0;
    process->exec_ring = 0;
    process->r0_slot = -1;
    strncpy(process->name, name ? name : "r0app", PROCESS_MAX_NAME - 1);
    process->name[PROCESS_MAX_NAME - 1] = '\0';
    return process;
}

void process_register(process_t* process) {
    if (!process || process->pid != 0) return;
    process->pid = g_next_pid++;
    if (process->pid == 0) process->pid = g_next_pid++;
    process_link(process);
}

thread_t* process_create_user_thread(process_t* process, uint64_t entry,
                                     uint64_t user_rsp) {
    if (!process || entry >= NEKO_USER_LIMIT || user_rsp >= NEKO_USER_LIMIT) {
        return NULL;
    }

    thread_t* thread = (thread_t*)kzalloc(sizeof(thread_t));
    if (!thread) return NULL;

    thread->kernel_stack_phys = pmm_alloc_pages(THREAD_KERNEL_STACK_PAGES);
    if (!thread->kernel_stack_phys) {
        kfree(thread);
        return NULL;
    }

    thread->kernel_stack_pages = THREAD_KERNEL_STACK_PAGES;
    uint8_t* stack = (uint8_t*)phys_to_virt(thread->kernel_stack_phys);
    memset(stack, 0, THREAD_KERNEL_STACK_PAGES * PAGE_SIZE);
    thread->kernel_stack_top =
        (uint64_t)stack + THREAD_KERNEL_STACK_PAGES * PAGE_SIZE;

    trap_frame_t* frame = (trap_frame_t*)(thread->kernel_stack_top -
                                           sizeof(trap_frame_t));
    memset(frame, 0, sizeof(trap_frame_t));
    frame->int_no = SYSCALL_VECTOR_MARKER;
    frame->rip = entry;
    frame->cs = GDT_USER_CODE_SEL;
    frame->rflags = 0x202;
    frame->rsp = user_rsp;
    frame->ss = GDT_USER_DATA_SEL;

    log_printf("[PROC] Ring 3 thread ready for %s: RIP=0x%016llx RSP=0x%016llx\n",
               process->name,
               (unsigned long long)frame->rip,
               (unsigned long long)frame->rsp);

    thread->tid = g_next_tid++;
    thread->process = process;
    thread->state = THREAD_READY;
    thread->is_user = true;
    thread->wait_win_id = -1;
    thread->quantum = SCHEDULER_QUANTUM_TICKS;
    thread->resume_frame = frame;
    fpu_init_state(thread->fpu_state);

    process->mem_pages_used += THREAD_KERNEL_STACK_PAGES;
    process->main_thread = thread;
    process->state = PROCESS_READY;
    scheduler_add_thread(thread);
    return thread;
}

thread_t* process_create_r0_thread(process_t* process, uint64_t entry,
                                   int argc, const char* const* argv,
                                   uint64_t rdx_api, size_t stack_pages) {
    if (!process || !entry) return NULL;
    if (stack_pages < 16) stack_pages = 16; /* Minimum 64 KB stack for Ring 0 PXE */
    if (stack_pages > 256) stack_pages = 256;

    thread_t* thread = (thread_t*)kzalloc(sizeof(thread_t));
    if (!thread) return NULL;

    thread->kernel_stack_phys = pmm_alloc_pages(stack_pages);
    if (!thread->kernel_stack_phys) {
        kfree(thread);
        return NULL;
    }

    thread->kernel_stack_pages = stack_pages;
    uint8_t* stack = (uint8_t*)phys_to_virt(thread->kernel_stack_phys);
    memset(stack, 0, stack_pages * PAGE_SIZE);
    thread->kernel_stack_top = (uint64_t)stack + stack_pages * PAGE_SIZE;

    uint64_t sp = thread->kernel_stack_top;
    if (argc < 0) argc = 0;
    if (argc > 16) argc = 16;

    uint64_t argv_ptrs[17];
    memset(argv_ptrs, 0, sizeof(argv_ptrs));

    if (argc == 0) {
        sp -= 32;
        strcpy((char*)sp, process->name[0] ? process->name : "r0app");
        argv_ptrs[0] = sp;
        argc = 1;
    } else {
        for (int i = 0; i < argc; i++) {
            const char* s = (argv && argv[i]) ? argv[i] : "";
            size_t slen = strlen(s) + 1;
            if (slen > 256) slen = 256;
            sp -= 256;
            memcpy((void*)sp, s, slen - 1);
            ((char*)sp)[slen - 1] = '\0';
            argv_ptrs[i] = sp;
        }
    }
    argv_ptrs[argc] = 0;

    sp = (sp - 16) & ~0xFULL;
    size_t ptr_bytes = (size_t)(argc + 1) * sizeof(uint64_t);
    sp = (sp - ptr_bytes) & ~0xFULL;
    memcpy((void*)sp, argv_ptrs, ptr_bytes);
    uint64_t argv_base = sp;

    uint64_t initial_rsp = (sp - 32) & ~0xFULL;
    trap_frame_t* frame =
        (trap_frame_t*)((initial_rsp - sizeof(trap_frame_t) - 64) & ~0xFULL);
    memset(frame, 0, sizeof(trap_frame_t));
    frame->rip = entry;
    frame->rdi = (uint64_t)argc;
    frame->rsi = argv_base;
    frame->rdx = rdx_api;
    frame->cs = GDT_KERNEL_CODE_SEL;
    frame->rflags = 0x202;
    frame->rsp = initial_rsp;
    frame->ss = GDT_KERNEL_DATA_SEL;

    thread->tid = g_next_tid++;
    thread->process = process;
    thread->state = THREAD_READY;
    thread->is_user = false;
    thread->wait_win_id = -1;
    thread->quantum = SCHEDULER_QUANTUM_TICKS;
    thread->resume_frame = frame;
    fpu_init_state(thread->fpu_state);

    process->mem_pages_used += stack_pages;
    process->main_thread = thread;
    process->state = PROCESS_READY;
    scheduler_add_thread(thread);
    return thread;
}

process_t* process_create_idle(void) {
    process_t* process = (process_t*)kzalloc(sizeof(process_t));
    thread_t* thread = (thread_t*)kzalloc(sizeof(thread_t));
    if (!process || !thread) {
        kfree(process);
        kfree(thread);
        return NULL;
    }

    process->pid = 0;
    process->ppid = 0;
    process->state = PROCESS_READY;
    process->exec_ring = 0;
    process->is_system_daemon = true;
    process->r0_slot = -1;
    process->pml4 = vmm_get_kernel_pml4();
    if (!virt_to_phys_checked(process->pml4, &process->pml4_phys)) {
        kfree(process);
        kfree(thread);
        return NULL;
    }
    strcpy(process->name, "idle");
    strcpy(process->exe_path, "[kernel:idle]");

    thread->kernel_stack_phys = pmm_alloc_pages(THREAD_KERNEL_STACK_PAGES);
    if (!thread->kernel_stack_phys) {
        kfree(process);
        kfree(thread);
        return NULL;
    }
    thread->kernel_stack_pages = THREAD_KERNEL_STACK_PAGES;
    uint8_t* stack = (uint8_t*)phys_to_virt(thread->kernel_stack_phys);
    memset(stack, 0, THREAD_KERNEL_STACK_PAGES * PAGE_SIZE);
    thread->kernel_stack_top =
        (uint64_t)stack + THREAD_KERNEL_STACK_PAGES * PAGE_SIZE;

    trap_frame_t* frame = (trap_frame_t*)(thread->kernel_stack_top -
                                           sizeof(trap_frame_t));
    memset(frame, 0, sizeof(trap_frame_t));
    frame->rip = (uint64_t)scheduler_idle_entry;
    frame->cs = GDT_KERNEL_CODE_SEL;
    frame->rflags = 0x202;
    frame->rsp = (thread->kernel_stack_top - sizeof(trap_frame_t) - 64) & ~0xFULL;
    frame->ss = GDT_KERNEL_DATA_SEL;

    thread->tid = 0;
    thread->process = process;
    thread->state = THREAD_READY;
    thread->is_idle = true;
    thread->is_user = false;
    thread->wait_win_id = -1;
    thread->quantum = SCHEDULER_QUANTUM_TICKS;
    thread->resume_frame = frame;
    fpu_init_state(thread->fpu_state);

    process->mem_pages_used = THREAD_KERNEL_STACK_PAGES;
    process->main_thread = thread;
    process_link(process);
    scheduler_add_thread(thread);
    return process;
}

process_t* process_create_compositor(void) {
    process_t* process = (process_t*)kzalloc(sizeof(process_t));
    thread_t* thread = (thread_t*)kzalloc(sizeof(thread_t));
    if (!process || !thread) {
        kfree(process);
        kfree(thread);
        return NULL;
    }

    process->state = PROCESS_READY;
    process->exec_ring = 0;
    process->is_system_daemon = true;
    process->r0_slot = -1;
    process->pml4 = vmm_get_kernel_pml4();
    if (!virt_to_phys_checked(process->pml4, &process->pml4_phys)) {
        kfree(process);
        kfree(thread);
        return NULL;
    }
    strcpy(process->name, "kwm_compositor");
    strcpy(process->exe_path, "[kernel:compositor]");

    size_t comp_stack_pages = 16; /* 64 KB stack for WM, VFS media poll & modal dialogs */
    thread->kernel_stack_phys = pmm_alloc_pages(comp_stack_pages);
    if (!thread->kernel_stack_phys) {
        kfree(process);
        kfree(thread);
        return NULL;
    }
    thread->kernel_stack_pages = comp_stack_pages;
    uint8_t* stack = (uint8_t*)phys_to_virt(thread->kernel_stack_phys);
    memset(stack, 0, comp_stack_pages * PAGE_SIZE);
    thread->kernel_stack_top = (uint64_t)stack + comp_stack_pages * PAGE_SIZE;

    uint64_t initial_rsp = (thread->kernel_stack_top - 64) & ~0xFULL;
    trap_frame_t* frame =
        (trap_frame_t*)((initial_rsp - sizeof(trap_frame_t) - 64) & ~0xFULL);
    memset(frame, 0, sizeof(trap_frame_t));
    frame->rip = (uint64_t)scheduler_compositor_entry;
    frame->cs = GDT_KERNEL_CODE_SEL;
    frame->rflags = 0x202;
    frame->rsp = initial_rsp;
    frame->ss = GDT_KERNEL_DATA_SEL;

    thread->tid = g_next_tid++;
    thread->process = process;
    thread->state = THREAD_READY;
    thread->is_idle = false;
    thread->is_compositor = true;
    thread->is_user = false;
    thread->wait_win_id = -1;
    thread->quantum = 2;
    thread->resume_frame = frame;
    fpu_init_state(thread->fpu_state);

    process->mem_pages_used = comp_stack_pages;
    process->main_thread = thread;
    process_register(process);
    scheduler_add_thread(thread);
    log_printf("[PROC] High-priority desktop compositor daemon ready (PID %u).\n",
               process->pid);
    return process;
}

process_t* process_create_user(const uint8_t* elf, size_t size,
                               const char* name, const char* arg0,
                               const char* arg1, process_t* parent) {
    return elf_load_process(elf, size, name, arg0, arg1, parent);
}

void process_set_current(process_t* process) {
    g_current_process = process;
}

process_t* process_current(void) {
    return g_current_process;
}

thread_t* process_current_thread(void) {
    return scheduler_current_thread();
}

uint32_t process_current_pid(void) {
    return g_current_process ? g_current_process->pid : 0;
}

process_t* process_find_by_pid(uint32_t pid) {
    for (process_t* p = g_process_list; p; p = p->next) {
        if (p->pid == pid) return p;
    }
    return NULL;
}

thread_t* process_find_thread_by_tid(uint64_t tid) {
    for (process_t* p = g_process_list; p; p = p->next) {
        if (p->main_thread && p->main_thread->tid == tid) return p->main_thread;
    }
    return NULL;
}

bool process_is_descendant(process_t* parent, process_t* child) {
    for (process_t* p = child; p && p != parent; p = p->parent) {
        if (p->parent == parent) return true;
    }
    return false;
}

phys_addr_t process_user_phys(process_t* process, uint64_t virt) {
    if (!process || !process->pml4 || !process_user_range_ok(process, virt, 1, false)) {
        return 0;
    }

    phys_addr_t phys = 0;
    return vmm_virt_to_phys(process->pml4, virt, &phys) ? phys : 0;
}

bool process_user_range_ok(process_t* process, uint64_t ptr,
                           size_t size, bool write) {
    return process && process->pml4 &&
           vmm_user_range_ok(process->pml4, ptr, size, write);
}

bool process_copy_to_user(process_t* process, uint64_t dst,
                          const void* src, size_t size) {
    if (size == 0) return true;
    if (!src || !process_user_range_ok(process, dst, size, true)) return false;

    const uint8_t* source = (const uint8_t*)src;
    size_t copied = 0;
    while (copied < size) {
        phys_addr_t phys = 0;
        if (!vmm_virt_to_phys(process->pml4, dst + copied, &phys)) return false;
        size_t page_left = PAGE_SIZE - ((dst + copied) & 0xFFFULL);
        size_t chunk = size - copied;
        if (chunk > page_left) chunk = page_left;
        memcpy(phys_to_virt(phys), source + copied, chunk);
        copied += chunk;
    }
    return true;
}

bool process_copy_from_user(process_t* process, void* dst,
                            uint64_t src, size_t size) {
    if (size == 0) return true;
    if (!dst || !process_user_range_ok(process, src, size, false)) return false;

    uint8_t* dest = (uint8_t*)dst;
    size_t copied = 0;
    while (copied < size) {
        phys_addr_t phys = 0;
        if (!vmm_virt_to_phys(process->pml4, src + copied, &phys)) return false;
        size_t page_left = PAGE_SIZE - ((src + copied) & 0xFFFULL);
        size_t chunk = size - copied;
        if (chunk > page_left) chunk = page_left;
        memcpy(dest + copied, phys_to_virt(phys), chunk);
        copied += chunk;
    }
    return true;
}

bool process_map_shared(process_t* process, phys_addr_t phys,
                        uint64_t virt, size_t size) {
    if (!process || !phys || size == 0) return false;

    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    return vmm_map_range(process->pml4, virt, phys, pages * PAGE_SIZE,
                         PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX);
}

int64_t process_sbrk(process_t* process, int64_t delta) {
    if (!process || process->exec_ring != 3 || !process->pml4) {
        return -1;
    }
    if (delta == 0) {
        return (int64_t)process->heap_brk;
    }

    uint64_t old_brk = process->heap_brk;
    uint64_t new_brk = old_brk;
    if (delta > 0) {
        uint64_t udelta = (uint64_t)delta;
        if (udelta > process->heap_limit - old_brk) {
            return -1;
        }
        new_brk = old_brk + udelta;
        uint64_t map_start = (old_brk + PAGE_SIZE - 1) & PAGE_MASK;
        uint64_t map_end   = (new_brk + PAGE_SIZE - 1) & PAGE_MASK;
        for (uint64_t va = map_start; va < map_end; va += PAGE_SIZE) {
            phys_addr_t existing = 0;
            if (vmm_virt_to_phys(process->pml4, va, &existing)) continue;
            phys_addr_t page = pmm_alloc_page();
            if (!page) {
                for (uint64_t roll = map_start; roll < va; roll += PAGE_SIZE) {
                    phys_addr_t rphys = 0;
                    if (vmm_virt_to_phys(process->pml4, roll, &rphys)) {
                        vmm_unmap_page(process->pml4, roll);
                        pmm_free_page(rphys);
                        if (process->mem_pages_used > 0) process->mem_pages_used--;
                    }
                }
                return -1;
            }
            memset(phys_to_virt(page), 0, PAGE_SIZE);
            if (!vmm_map_page(process->pml4, va, page,
                              PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX)) {
                pmm_free_page(page);
                return -1;
            }
            process->mem_pages_used++;
        }
    } else {
        uint64_t udelta = (uint64_t)(-delta);
        if (udelta > old_brk - process->heap_base) {
            new_brk = process->heap_base;
        } else {
            new_brk = old_brk - udelta;
        }
        uint64_t unmap_start = (new_brk + PAGE_SIZE - 1) & PAGE_MASK;
        uint64_t unmap_end   = (old_brk + PAGE_SIZE - 1) & PAGE_MASK;
        for (uint64_t va = unmap_start; va < unmap_end; va += PAGE_SIZE) {
            phys_addr_t phys = 0;
            if (vmm_virt_to_phys(process->pml4, va, &phys)) {
                vmm_unmap_page(process->pml4, va);
                pmm_free_page(phys);
                if (process->mem_pages_used > 0) process->mem_pages_used--;
            }
        }
    }

    process->heap_brk = new_brk;
    return (int64_t)old_brk;
}

void process_cleanup_runtime_resources(process_t* process) {
    if (!process || process->runtime_cleaned) return;
    process->runtime_cleaned = true;

    for (int i = 0; i < PROCESS_MAX_STREAMS; i++) {
        if (process->streams[i]) {
            vfs_stream_t* s = (vfs_stream_t*)process->streams[i];
            if (s->is_open) vfs_stream_close(s);
            kfree(s);
            process->streams[i] = NULL;
        }
    }

    wm_pxe_destroy_all_for_pid(process->pid);

    r0_alloc_node_t* cur = process->r0_allocs;
    while (cur) {
        r0_alloc_node_t* nxt = cur->next;
        kfree(cur);
        cur = nxt;
    }
    process->r0_allocs = NULL;
    process->r0_heap_bytes = 0;
}

void process_exit_current(int status) {
    process_t* process = g_current_process;
    thread_t* thread = scheduler_current_thread();
    if (!process || process->is_system_daemon) return;

    process_cleanup_runtime_resources(process);

    process->state = PROCESS_ZOMBIE;
    process->exit_status = status;
    process->zombie_reap_tick = scheduler_get_ticks() + 1000;
    if (thread) {
        thread->state = THREAD_ZOMBIE;
        thread->wake_tick = 0;
        thread->wait_queue_id = 0;
        thread->wait_win_id = -1;
    }

    log_printf("[PROC] PID %u (%s) exited with status %d.\n",
               process->pid, process->name, status);
    scheduler_request_reschedule();
}

int process_kill(uint32_t pid, int exit_code) {
    process_t* target = process_find_by_pid(pid);
    if (!target || target->pid == 0 || target->is_system_daemon) {
        return -1;
    }
    if (target == g_current_process) {
        process_exit_current(exit_code);
        return 0;
    }
    if (target->state == PROCESS_ZOMBIE) {
        return -1;
    }

    process_cleanup_runtime_resources(target);

    target->state = PROCESS_ZOMBIE;
    target->exit_status = exit_code;
    target->zombie_reap_tick = scheduler_get_ticks() + 200;
    if (target->main_thread) {
        target->main_thread->state = THREAD_ZOMBIE;
        target->main_thread->wake_tick = 0;
        target->main_thread->wait_queue_id = 0;
        target->main_thread->wait_win_id = -1;
    }

    log_printf("[PROC] PID %u (%s) terminated via process_kill (code %d).\n",
               target->pid, target->name, exit_code);
    return 0;
}
