#ifndef BOARDCOMM_TYPES_H
#define BOARDCOMM_TYPES_H

#include <stddef.h>
#include <stdint.h>

#define BC_MAX_CHANNEL_LEN 32
#define BC_MAX_TOPIC_LEN 64
#define BC_MAX_PAYLOAD_LEN 1400
#define BC_DEFAULT_SOCKET_PATH "/tmp/boardcommd.sock"

#define BC_FILE_PKT_MAGIC 0xBCF1u
#define BC_FILE_NAME_MAX 255
#define BC_FILE_CHUNK_DATA_MAX (BC_MAX_PAYLOAD_LEN - 8u)

typedef enum {
    BC_FILE_PKT_START = 1,
    BC_FILE_PKT_DATA = 2,
    BC_FILE_PKT_END = 3,
} bc_file_pkt_type_t;

typedef enum {
    BC_OK = 0,
    BC_ERR = -1,
    BC_ERR_INVALID = -2,
    BC_ERR_IO = -3,
    BC_ERR_NOMEM = -4,
    BC_ERR_NOT_FOUND = -5,
    BC_ERR_TIMEOUT = -6,
} bc_status_t;

typedef struct {
    uint32_t src_node;
    uint32_t dst_node;
    uint16_t flags;
    uint16_t qos;
    uint32_t seq;
    char channel[BC_MAX_CHANNEL_LEN];
    char topic[BC_MAX_TOPIC_LEN];
    size_t payload_len;
    uint8_t payload[BC_MAX_PAYLOAD_LEN];
} bc_message_t;

typedef void (*bc_msg_cb)(
    const char *topic,
    const void *payload,
    size_t len,
    void *user);

#endif
