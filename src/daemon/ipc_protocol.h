#ifndef BC_IPC_PROTOCOL_H
#define BC_IPC_PROTOCOL_H

#include <stdint.h>

#include "boardcomm_types.h"

#define BC_IPC_MAGIC 0x42434950u

typedef enum {
    BC_IPC_PUBLISH = 1,
    BC_IPC_SUBSCRIBE = 2,
    BC_IPC_DELIVER = 3,
} bc_ipc_type_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint16_t topic_len;
    uint16_t channel_len;
    uint32_t payload_len;
} bc_ipc_header_t;

#endif
