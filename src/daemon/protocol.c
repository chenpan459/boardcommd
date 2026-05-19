#include "protocol.h"

#include <string.h>

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_len;
    uint32_t src_node;
    uint32_t dst_node;
    uint16_t flags;
    uint16_t qos;
    uint32_t seq;
    uint16_t topic_len;
    uint16_t reserved;
    uint32_t payload_len;
    uint32_t checksum;
} bc_frame_header_t;

static uint32_t bc_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xffffffffu;

    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t mask = 0u - (crc & 1u);

            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

int bc_protocol_encode(const bc_message_t *msg, bc_frame_t *frame)
{
    bc_frame_header_t header;
    size_t topic_len;
    size_t total_len;

    if (msg == NULL || frame == NULL) {
        return BC_ERR_INVALID;
    }

    topic_len = strnlen(msg->topic, BC_MAX_TOPIC_LEN);
    if (topic_len == 0 || topic_len >= BC_MAX_TOPIC_LEN || msg->payload_len > BC_MAX_PAYLOAD_LEN) {
        return BC_ERR_INVALID;
    }

    total_len = sizeof(header) + topic_len + msg->payload_len;
    if (total_len > sizeof(frame->data)) {
        return BC_ERR_INVALID;
    }

    memset(&header, 0, sizeof(header));
    header.magic = BC_FRAME_MAGIC;
    header.version = BC_FRAME_VERSION;
    header.header_len = sizeof(header);
    header.src_node = msg->src_node;
    header.dst_node = msg->dst_node;
    header.flags = msg->flags;
    header.qos = msg->qos;
    header.seq = msg->seq;
    header.topic_len = (uint16_t)topic_len;
    header.payload_len = (uint32_t)msg->payload_len;
    header.checksum = bc_crc32(msg->payload, msg->payload_len);

    memcpy(frame->data, &header, sizeof(header));
    memcpy(frame->data + sizeof(header), msg->topic, topic_len);
    memcpy(frame->data + sizeof(header) + topic_len, msg->payload, msg->payload_len);
    frame->len = total_len;

    return BC_OK;
}

int bc_protocol_frame_length(const uint8_t *data, size_t len, size_t *frame_len)
{
    bc_frame_header_t header;
    size_t total_len;

    if (data == NULL || frame_len == NULL) {
        return BC_ERR_INVALID;
    }
    if (len < sizeof(header)) {
        return BC_ERR_NOT_FOUND;
    }

    memcpy(&header, data, sizeof(header));
    if (header.magic != BC_FRAME_MAGIC || header.version != BC_FRAME_VERSION) {
        return BC_ERR_INVALID;
    }
    if (header.header_len != sizeof(header)) {
        return BC_ERR_INVALID;
    }
    if (header.topic_len == 0 || header.topic_len >= BC_MAX_TOPIC_LEN) {
        return BC_ERR_INVALID;
    }
    if (header.payload_len > BC_MAX_PAYLOAD_LEN) {
        return BC_ERR_INVALID;
    }

    total_len = sizeof(header) + header.topic_len + header.payload_len;
    if (total_len > BC_MAX_FRAME_LEN) {
        return BC_ERR_INVALID;
    }

    *frame_len = total_len;
    return len >= total_len ? BC_OK : BC_ERR_NOT_FOUND;
}

int bc_protocol_decode(const uint8_t *data, size_t len, bc_message_t *msg)
{
    bc_frame_header_t header;
    const uint8_t *payload;

    if (data == NULL || msg == NULL || len < sizeof(header)) {
        return BC_ERR_INVALID;
    }

    memcpy(&header, data, sizeof(header));
    if (header.magic != BC_FRAME_MAGIC || header.version != BC_FRAME_VERSION) {
        return BC_ERR_INVALID;
    }
    if (header.topic_len == 0 || header.topic_len >= BC_MAX_TOPIC_LEN) {
        return BC_ERR_INVALID;
    }
    if (header.payload_len > BC_MAX_PAYLOAD_LEN) {
        return BC_ERR_INVALID;
    }
    if (header.header_len != sizeof(header)) {
        return BC_ERR_INVALID;
    }
    if (len < sizeof(header) + header.topic_len + header.payload_len) {
        return BC_ERR_INVALID;
    }

    payload = data + sizeof(header) + header.topic_len;
    if (bc_crc32(payload, header.payload_len) != header.checksum) {
        return BC_ERR_INVALID;
    }

    memset(msg, 0, sizeof(*msg));
    msg->src_node = header.src_node;
    msg->dst_node = header.dst_node;
    msg->flags = header.flags;
    msg->qos = header.qos;
    msg->seq = header.seq;
    memcpy(msg->topic, data + sizeof(header), header.topic_len);
    msg->topic[header.topic_len] = '\0';
    msg->payload_len = header.payload_len;
    memcpy(msg->payload, payload, header.payload_len);

    return BC_OK;
}
