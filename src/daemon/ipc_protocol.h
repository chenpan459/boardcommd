#ifndef BC_IPC_PROTOCOL_H
#define BC_IPC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include "bc_types.h"

#define BC_IPC_MAGIC 0x42434950u

typedef enum {
    BC_IPC_PUBLISH = 1,
    BC_IPC_SUBSCRIBE = 2,
    BC_IPC_DELIVER = 3,
    BC_IPC_STATS = 4,
    BC_IPC_SHM_SETUP = 5,
    BC_IPC_SHM_KICK = 6,
} bc_ipc_type_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint16_t topic_len;
    uint16_t channel_len;
    uint32_t payload_len;
    uint32_t dst_node;
    uint16_t qos;
    uint16_t flags;
    uint32_t seq;
} bc_ipc_header_t;

static inline size_t bc_ipc_header_size(uint16_t version)
{
    if (version >= BC_IPC_VERSION) {
        return sizeof(bc_ipc_header_t);
    }
    return offsetof(bc_ipc_header_t, dst_node);
}

#endif
