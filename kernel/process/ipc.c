#include "ipc.h"
#include "scheduler.h"
#include "percpu.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "log.h"

typedef struct {
    bool used;
    uint64_t id;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    neko_message_t messages[IPC_QUEUE_DEPTH];
} ipc_queue_t;

typedef struct {
    bool used;
    uint64_t id;
    uint64_t size;
    uint32_t page_count;
    phys_addr_t phys;
} ipc_shm_t;

static ipc_queue_t g_queues[IPC_MAX_QUEUES];
static ipc_shm_t g_shm_regions[IPC_MAX_SHM];

static ipc_queue_t* find_queue(uint64_t id) {
    for (uint32_t i = 0; i < IPC_MAX_QUEUES; i++) {
        if (g_queues[i].used && g_queues[i].id == id) return &g_queues[i];
    }
    return NULL;
}

static ipc_shm_t* find_shm(uint64_t id) {
    for (uint32_t i = 0; i < IPC_MAX_SHM; i++) {
        if (g_shm_regions[i].used && g_shm_regions[i].id == id) {
            return &g_shm_regions[i];
        }
    }
    return NULL;
}

static bool create_queue(uint64_t id) {
    if (find_queue(id)) return true;
    for (uint32_t i = 0; i < IPC_MAX_QUEUES; i++) {
        if (!g_queues[i].used) {
            memset(&g_queues[i], 0, sizeof(g_queues[i]));
            g_queues[i].used = true;
            g_queues[i].id = id;
            return true;
        }
    }
    return false;
}

static bool create_shm(uint64_t id, size_t size) {
    if (!size || find_shm(id)) return false;
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    phys_addr_t phys = pmm_alloc_pages(pages);
    if (!phys) return false;

    memset(phys_to_virt(phys), 0, pages * PAGE_SIZE);
    for (uint32_t i = 0; i < IPC_MAX_SHM; i++) {
        if (!g_shm_regions[i].used) {
            g_shm_regions[i].used = true;
            g_shm_regions[i].id = id;
            g_shm_regions[i].size = pages * PAGE_SIZE;
            g_shm_regions[i].page_count = (uint32_t)pages;
            g_shm_regions[i].phys = phys;
            return true;
        }
    }
    pmm_free_pages(phys, pages);
    return false;
}

void ipc_init(void) {
    memset(g_queues, 0, sizeof(g_queues));
    memset(g_shm_regions, 0, sizeof(g_shm_regions));

    bool queue_ok = create_queue(NEKO_TEST_QUEUE_ID);
    bool response_queue_ok = create_queue(NEKO_TEST_RESPONSE_QUEUE_ID);
    bool shm_ok = create_shm(NEKO_TEST_SHM_ID, PAGE_SIZE);
    log_printf("[IPC] Request queue %llu: %s; response queue %llu: %s; shared memory %llu: %s.\n",
               (unsigned long long)NEKO_TEST_QUEUE_ID, queue_ok ? "ready" : "FAILED",
               (unsigned long long)NEKO_TEST_RESPONSE_QUEUE_ID,
               response_queue_ok ? "ready" : "FAILED",
               (unsigned long long)NEKO_TEST_SHM_ID, shm_ok ? "ready" : "FAILED");
}

static bool enqueue(ipc_queue_t* queue, const neko_message_t* message) {
    if (!queue || !message || queue->count >= IPC_QUEUE_DEPTH) return false;
    queue->messages[queue->tail] = *message;
    queue->tail = (queue->tail + 1) % IPC_QUEUE_DEPTH;
    queue->count++;
    return true;
}

static bool dequeue(ipc_queue_t* queue, neko_message_t* message) {
    if (!queue || !message || queue->count == 0) return false;
    *message = queue->messages[queue->head];
    queue->head = (queue->head + 1) % IPC_QUEUE_DEPTH;
    queue->count--;
    return true;
}

void ipc_wake_next_receiver(uint64_t queue_id) {
    ipc_queue_t* queue = find_queue(queue_id);
    thread_t* waiter = scheduler_find_ipc_waiter(queue_id);
    if (!queue || !waiter || queue->count == 0) return;

    neko_message_t message;
    if (!dequeue(queue, &message)) return;

    bool delivered = process_copy_to_user(waiter->process,
                                          waiter->wait_user_dst,
                                          &message, sizeof(message));
    waiter->wait_queue_id = 0;
    waiter->wait_user_dst = 0;
    if (waiter->resume_frame) {
        waiter->resume_frame->rax = delivered ? 0 : (uint64_t)-NEKO_EFAULT;
    }
    scheduler_wake_thread(waiter);

    log_printf("[IPC] Delivered queued message to PID %u (%s).\n",
               waiter->process ? waiter->process->pid : 0,
               delivered ? "ok" : "EFAULT");
}

int64_t ipc_send(uint64_t queue_id, const neko_message_t* message,
                 uint32_t sender_pid) {
    ipc_queue_t* queue = find_queue(queue_id);
    log_printf("[IPC] PID %u send queue=%llu type=0x%llx\n",
               sender_pid, (unsigned long long)queue_id,
               message ? (unsigned long long)message->type : 0);
    if (!queue) return -NEKO_ENOENT;
    if (!message) return -NEKO_EINVAL;

    neko_message_t copy = *message;
    copy.sender_pid = sender_pid;
    if (!enqueue(queue, &copy)) return -NEKO_ENOSPC;

    ipc_wake_next_receiver(queue_id);
    return 0;
}

int64_t ipc_recv(uint64_t queue_id, neko_message_t* message) {
    ipc_queue_t* queue = find_queue(queue_id);
    if (!queue) return -NEKO_ENOENT;
    if (!message) return -NEKO_EINVAL;
    return dequeue(queue, message) ? 0 : -NEKO_EAGAIN;
}

int64_t ipc_recv_block(process_t* process, thread_t* thread, uint64_t queue_id,
                       uint64_t user_dst) {
    if (!process || !thread || !process_user_range_ok(process, user_dst,
                                                       sizeof(neko_message_t), true)) {
        return -NEKO_EFAULT;
    }

    ipc_queue_t* queue = find_queue(queue_id);
    if (!queue) {
        log_printf("[IPC] PID %u receive missing queue=%llu\n",
                   process->pid, (unsigned long long)queue_id);
        return -NEKO_ENOENT;
    }

    neko_message_t message;
    if (dequeue(queue, &message)) {
        log_printf("[IPC] PID %u received queue=%llu immediately.\n",
                   process->pid, (unsigned long long)queue_id);
        return process_copy_to_user(process, user_dst, &message, sizeof(message))
                   ? 0
                   : -NEKO_EFAULT;
    }

    log_printf("[IPC] PID %u blocking on queue=%llu dst=0x%016llx\n",
               process->pid, (unsigned long long)queue_id,
               (unsigned long long)user_dst);
    thread->wait_queue_id = queue_id;
    thread->wait_user_dst = user_dst;
    thread->state = THREAD_BLOCKED;
    process->state = PROCESS_BLOCKED;
    per_cpu_request_reschedule();
    return 0;
}

uint64_t ipc_shm_size(uint64_t shm_id) {
    ipc_shm_t* shm = find_shm(shm_id);
    return shm ? shm->size : 0;
}

int64_t ipc_shm_attach(process_t* process, uint64_t shm_id) {
    if (!process) return -NEKO_EINVAL;
    ipc_shm_t* shm = find_shm(shm_id);
    if (!shm) return -NEKO_ENOENT;

    uint64_t virt = NEKO_SHM_BASE + shm_id * NEKO_SHM_STRIDE;
    if (virt >= NEKO_USER_LIMIT ||
        shm->size > NEKO_USER_LIMIT - virt) {
        return -NEKO_EINVAL;
    }

    if (!process_user_phys(process, virt)) {
        if (!process_map_shared(process, shm->phys, virt, shm->size)) {
            return -NEKO_ENOMEM;
        }
    }

    return (int64_t)virt;
}
