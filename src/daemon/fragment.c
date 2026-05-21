#include "fragment.h"

#include <string.h>

static bc_reasm_slot_t *find_slot(bc_reasm_t *reasm, uint32_t seq, uint32_t src_node)
{
    bc_reasm_slot_t *free_slot = NULL;
    size_t i;

    for (i = 0; i < BC_REASM_SLOTS; i++) {
        bc_reasm_slot_t *slot = &reasm->slots[i];
        if (slot->active && slot->seq == seq && slot->src_node == src_node) {
            return slot;
        }
        if (!slot->active && free_slot == NULL) {
            free_slot = slot;
        }
    }
    return free_slot;
}

void bc_reasm_init(bc_reasm_t *reasm)
{
    if (reasm != NULL) {
        memset(reasm, 0, sizeof(*reasm));
    }
}

int bc_reasm_feed(bc_reasm_t *reasm, const bc_message_t *frag, bc_message_t *out)
{
    uint16_t index;
    uint16_t total;
    size_t chunk_len;
    size_t offset;
    bc_reasm_slot_t *slot;

    if (reasm == NULL || frag == NULL || out == NULL) {
        return BC_ERR_INVALID;
    }
    if (frag->payload_len < BC_FRAG_HDR_SIZE) {
        return BC_ERR_INVALID;
    }

    index = (uint16_t)frag->payload[0] | ((uint16_t)frag->payload[1] << 8);
    total = (uint16_t)frag->payload[2] | ((uint16_t)frag->payload[3] << 8);
    if (total == 0 || index >= total) {
        return BC_ERR_INVALID;
    }

    slot = find_slot(reasm, frag->seq, frag->src_node);
    if (slot == NULL) {
        return BC_ERR_NOMEM;
    }

    if (!slot->active) {
        slot->active = 1;
        slot->seq = frag->seq;
        slot->total = total;
        slot->received = 0;
        slot->src_node = frag->src_node;
        slot->dst_node = frag->dst_node;
        slot->qos = frag->qos;
        slot->payload_len = 0;
        memset(slot->bitmap, 0, sizeof(slot->bitmap));
        strncpy(slot->topic, frag->topic, sizeof(slot->topic) - 1);
        strncpy(slot->channel, frag->channel, sizeof(slot->channel) - 1);
    } else if (slot->total != total) {
        return BC_ERR_INVALID;
    }

    chunk_len = frag->payload_len - BC_FRAG_HDR_SIZE;
    offset = (size_t)index * BC_FRAG_CHUNK_MAX;
    if (offset + chunk_len > BC_MAX_PAYLOAD_LEN) {
        return BC_ERR_INVALID;
    }
    if (slot->bitmap[index]) {
        return BC_ERR_NOT_FOUND;
    }

    memcpy(slot->payload + offset, frag->payload + BC_FRAG_HDR_SIZE, chunk_len);
    slot->bitmap[index] = 1;
    slot->received++;
    if (offset + chunk_len > slot->payload_len) {
        slot->payload_len = offset + chunk_len;
    }

    if (slot->received < slot->total) {
        return BC_ERR_NOT_FOUND;
    }

    memset(out, 0, sizeof(*out));
    out->src_node = slot->src_node;
    out->dst_node = slot->dst_node;
    out->qos = slot->qos;
    out->seq = slot->seq;
    strncpy(out->topic, slot->topic, sizeof(out->topic) - 1);
    strncpy(out->channel, slot->channel, sizeof(out->channel) - 1);
    out->payload_len = slot->payload_len;
    memcpy(out->payload, slot->payload, slot->payload_len);

    slot->active = 0;
    return BC_OK;
}

int bc_fragment_publish(
    const bc_message_t *msg,
    int (*emit)(const bc_message_t *piece, void *user),
    void *user)
{
    uint16_t total;
    uint16_t index;
    size_t offset;
    bc_message_t piece;

    if (msg == NULL || emit == NULL) {
        return BC_ERR_INVALID;
    }
    if (msg->payload_len <= BC_FRAG_CHUNK_MAX) {
        return emit(msg, user);
    }

    total = (uint16_t)((msg->payload_len + BC_FRAG_CHUNK_MAX - 1u) / BC_FRAG_CHUNK_MAX);
    if (total == 0) {
        return BC_ERR_INVALID;
    }

    for (index = 0; index < total; index++) {
        size_t chunk_len;
        int rc;

        offset = (size_t)index * BC_FRAG_CHUNK_MAX;
        chunk_len = msg->payload_len - offset;
        if (chunk_len > BC_FRAG_CHUNK_MAX) {
            chunk_len = BC_FRAG_CHUNK_MAX;
        }

        memset(&piece, 0, sizeof(piece));
        piece.src_node = msg->src_node;
        piece.dst_node = msg->dst_node;
        piece.qos = msg->qos;
        piece.seq = msg->seq;
        piece.flags = BC_FLAG_FRAGMENT;
        strncpy(piece.topic, msg->topic, sizeof(piece.topic) - 1);
        strncpy(piece.channel, msg->channel, sizeof(piece.channel) - 1);

        piece.payload[0] = (uint8_t)(index & 0xffu);
        piece.payload[1] = (uint8_t)((index >> 8) & 0xffu);
        piece.payload[2] = (uint8_t)(total & 0xffu);
        piece.payload[3] = (uint8_t)((total >> 8) & 0xffu);
        memcpy(piece.payload + BC_FRAG_HDR_SIZE, msg->payload + offset, chunk_len);
        piece.payload_len = BC_FRAG_HDR_SIZE + chunk_len;

        rc = emit(&piece, user);
        if (rc != BC_OK) {
            return rc;
        }
    }

    return BC_OK;
}
