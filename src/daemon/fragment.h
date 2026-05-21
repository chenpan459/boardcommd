#ifndef BC_FRAGMENT_H
#define BC_FRAGMENT_H

#include "bc_types.h"

#define BC_FRAG_HDR_SIZE 8u
#define BC_FRAG_CHUNK_MAX (BC_MAX_PAYLOAD_LEN - BC_FRAG_HDR_SIZE)
#define BC_REASM_SLOTS 16u

typedef struct {
    int active;
    uint32_t seq;
    uint16_t total;
    uint16_t received;
    uint16_t bitmap[BC_MAX_PAYLOAD_LEN / BC_FRAG_CHUNK_MAX + 1];
    char topic[BC_MAX_TOPIC_LEN];
    char channel[BC_MAX_CHANNEL_LEN];
    uint32_t src_node;
    uint32_t dst_node;
    uint16_t qos;
    uint8_t payload[BC_MAX_PAYLOAD_LEN];
    size_t payload_len;
} bc_reasm_slot_t;

typedef struct {
    bc_reasm_slot_t slots[BC_REASM_SLOTS];
} bc_reasm_t;

void bc_reasm_init(bc_reasm_t *reasm);
int bc_reasm_feed(bc_reasm_t *reasm, const bc_message_t *frag, bc_message_t *out);
int bc_fragment_publish(
    const bc_message_t *msg,
    int (*emit)(const bc_message_t *piece, void *user),
    void *user);

#endif
