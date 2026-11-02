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

static process_t* g_process_list = NULL;
static process_t* g_current_process = NULL;
static uint32_t g_next_pid = 1;
static uint64_t g_next_tid = 1;

static void process_link(process_t* process) {
    if (!process) return;
    process->next = g_process_list;
    g_process_list = process;
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
        kfree(process);
        return NULL;
    }

    process->state = PROCESS_READY;
    process->parent = parent;
    process->ppid = parent ? parent->pid : 0;
    process->exit_status = 0;
    strncpy(process->name, name ? name : "user", PROCESS_MAX_NAME - 1);
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

    thread->tid = g_next_tid++;
    thread->process = process;
    thread->state = THREAD_READY;
    thread->is_user = true;
    thread->quantum = SCHEDULER_QUANTUM_TICKS;
    thread->resume_frame = frame;
    fpu_init_state(thread->fpu_state);

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
    process->pml4 = vmm_get_kernel_pml4();
    if (!virt_to_phys_checked(process->pml4, &process->pml4_phys)) {
        kfree(process);
        kfree(thread);
        return NULL;
    }
    strcpy(process->name, "idle");

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

    thread->tid = 0;
    thread->process = process;
    thread->state = THREAD_READY;
    thread->is_idle = true;
    thread->is_user = false;
    thread->quantum = SCHEDULER_QUANTUM_TICKS;
    thread->resume_frame = frame;
    fpu_init_state(thread->fpu_state);

    process->main_thread = thread;
    process_link(process);
    scheduler_add_thread(thread);
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

void process_exit_current(int status) {
    process_t* process = g_current_process;
    thread_t* thread = scheduler_current_thread();
    if (!process) return;

    process->state = PROCESS_ZOMBIE;
    process->exit_status = status;
    if (thread) {
        thread->state = THREAD_ZOMBIE;
    }

    log_printf("[PROC] PID %u (%s) exited with status %d.\n",
               process->pid, process->name, status);
    scheduler_request_reschedule();
}
