#ifndef BC_PROTOCOL_H
#define BC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include "bc_types.h"

#define BC_FRAME_MAGIC 0x42434652u
#define BC_FRAME_VERSION 1u
#define BC_MAX_FRAME_LEN 1600

typedef struct {
    uint8_t data[BC_MAX_FRAME_LEN];
    size_t len;
} bc_frame_t;

int bc_protocol_encode(const bc_message_t *msg, bc_frame_t *frame);
int bc_protocol_frame_length(const uint8_t *data, size_t len, size_t *frame_len);
int bc_protocol_decode(const uint8_t *data, size_t len, bc_message_t *msg);

#endif
