#ifndef _PROCESS_H
#define _PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "vmm.h"
#include "idt.h"
#include "fpu.h"
#include "spinlock.h"

#define PROCESS_MAX_NAME          32
#define PROCESS_MAX_PATH          128
#define PROCESS_MAX_STREAMS       16
#define THREAD_KERNEL_STACK_PAGES 8

typedef enum {
    PROCESS_READY = 0,
    PROCESS_RUNNING,
    PROCESS_BLOCKED,
    PROCESS_ZOMBIE
} process_state_t;

typedef enum {
    THREAD_READY = 0,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_ZOMBIE
} thread_state_t;

typedef struct process process_t;
typedef struct thread thread_t;

typedef struct user_region {
    uint64_t virt_start;
    uint64_t size;
    phys_addr_t phys_start;
    uint64_t flags;
    struct user_region* next;
} user_region_t;

typedef struct r0_alloc_node {
    struct r0_alloc_node* next;
    struct r0_alloc_node* prev;
    size_t size;
    uint64_t magic;
} __attribute__((aligned(16))) r0_alloc_node_t;

struct thread {
    uint64_t tid;
    process_t* process;
    thread_state_t state;
    bool is_idle;
    bool is_compositor;
    bool is_user;
    bool joined;
    int  thread_exit_code;
    int32_t pinned_cpu;          /* -1 = any CPU, >= 0 = pinned to specific CPU */
    volatile int32_t running_cpu;/* -1 = not running, >= 0 = currently running on CPU */
    uint64_t quantum;
    uint64_t wake_tick;          /* 100 Hz tick deadline (0 = none) */
    uint64_t wake_ns;            /* Monotonic ns deadline (0 = none) */
    uint64_t wait_queue_id;
    uint64_t wait_user_dst;
    int32_t  wait_win_id;
    phys_addr_t futex_phys;      /* Physical address of futex word (0 = none) */
    bool     futex_timed_out;
    uint64_t join_target_tid;    /* TID being joined (0 = none) */
    uint64_t fs_base;            /* Ring 3 TLS FS_BASE MSR value */
    phys_addr_t kernel_stack_phys;
    size_t kernel_stack_pages;
    uint64_t kernel_stack_top;
    trap_frame_t* resume_frame;
    uint8_t fpu_state[FPU_STATE_SIZE] __attribute__((aligned(64)));
    thread_t* proc_next;         /* Next thread in same process_t */
    thread_t* next;              /* Next thread in global scheduler list */
};

struct process {
    uint32_t pid;
    uint32_t ppid;
    process_state_t state;
    process_t* parent;
    process_t* next;
    thread_t* main_thread;
    thread_t* threads_head;
    uint32_t thread_count;
    uint32_t alive_thread_count;
    spinlock_t lock;
    pagetable_t pml4;
    phys_addr_t pml4_phys;
    user_region_t* regions;
    int exit_status;

    /* PXE Runtime & Resource Tracking */
    uint8_t exec_ring;
    uint16_t pxe_flags;
    bool is_pxe;
    bool is_system_daemon;
    bool waited;
    bool runtime_cleaned;
    uint64_t zombie_reap_tick;
    volatile uint64_t cpu_ticks_total;
    uint64_t mem_pages_used;
    uint64_t r0_heap_bytes;

    /* Ring 3 User Heap & Shared Window Mapping */
    uint64_t heap_base;
    uint64_t heap_brk;
    uint64_t heap_limit;
    uint64_t next_shm_virt;

    /* Ring 0 PXE Relocatable Slot & Segment Tracking */
    int32_t r0_slot;
    uint64_t r0_mod_base;
    uint64_t r0_seg_vbase[4];
    phys_addr_t r0_seg_phys[4];
    size_t r0_seg_pages[4];
    r0_alloc_node_t* r0_allocs;

    /* Per-process open VFS stream pointers (vfs_stream_t*) */
    void* streams[PROCESS_MAX_STREAMS];

    char name[PROCESS_MAX_NAME];
    char exe_path[PROCESS_MAX_PATH];
};

void process_subsystem_init(void);
process_t* process_head(void);
void process_unlink(process_t* process);
process_t* process_create_idle(void);
thread_t* process_create_ap_idle(uint32_t cpu_id);
process_t* process_create_compositor(void);
process_t* process_create_empty(const char* name, process_t* parent);
process_t* process_create_r0_empty(const char* name, process_t* parent);
process_t* process_create_user(const uint8_t* elf, size_t size,
                               const char* name, const char* arg0,
                               const char* arg1, process_t* parent);
thread_t* process_create_user_thread(process_t* process, uint64_t entry,
                                     uint64_t user_rsp);
thread_t* process_create_user_subthread(process_t* process, uint64_t entry,
                                        uint64_t arg, uint64_t user_rsp,
                                        uint64_t tls_base);
thread_t* process_create_r0_thread(process_t* process, uint64_t entry,
                                   int argc, const char* const* argv,
                                   uint64_t rdx_api, size_t stack_pages);
void process_register(process_t* process);
void process_set_current(process_t* process);
process_t* process_current(void);
thread_t* process_current_thread(void);
uint32_t process_current_pid(void);
void process_cleanup_runtime_resources(process_t* process);
void process_exit_current(int status);
void process_thread_exit_current(int status);
int  process_thread_join(process_t* process, uint64_t target_tid, int* out_status);
int process_kill(uint32_t pid, int exit_code);
int process_kill_force(uint32_t pid, int exit_code);
int64_t process_sbrk(process_t* process, int64_t delta);
process_t* process_find_by_pid(uint32_t pid);
thread_t* process_find_thread_by_tid(uint64_t tid);
bool process_is_descendant(process_t* parent, process_t* child);
bool process_map_shared(process_t* process, phys_addr_t phys,
                        uint64_t virt, size_t size);
phys_addr_t process_user_phys(process_t* process, uint64_t virt);
bool process_copy_to_user(process_t* process, uint64_t dst,
                          const void* src, size_t size);
bool process_copy_from_user(process_t* process, void* dst,
                            uint64_t src, size_t size);
bool process_user_range_ok(process_t* process, uint64_t ptr,
                           size_t size, bool write);

#endif /* _PROCESS_H */
