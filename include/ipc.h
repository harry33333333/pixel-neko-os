#ifndef _IPC_H
#define _IPC_H

#include <stdint.h>
#include <stdbool.h>
#include "neko_abi.h"
#include "process.h"

#define IPC_MAX_QUEUES      8
#define IPC_QUEUE_DEPTH     16
#define IPC_MAX_SHM         8

void ipc_init(void);
int64_t ipc_send(uint64_t queue_id, const neko_message_t* message, uint32_t sender_pid);
int64_t ipc_recv(uint64_t queue_id, neko_message_t* message);
int64_t ipc_recv_block(process_t* process, thread_t* thread, uint64_t queue_id,
                       uint64_t user_dst);
int64_t ipc_shm_attach(process_t* process, uint64_t shm_id);
uint64_t ipc_shm_size(uint64_t shm_id);
void ipc_wake_next_receiver(uint64_t queue_id);

#endif /* _IPC_H */
