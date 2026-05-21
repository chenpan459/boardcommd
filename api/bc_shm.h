#ifndef BC_SHM_H
#define BC_SHM_H

#include <stddef.h>
#include <stdint.h>

#include "bc_types.h"

#define BC_SHM_RING_SIZE (32u * 1024u * 1024u)

typedef struct {
    volatile uint32_t head;
    volatile uint32_t tail;
    uint32_t size;
    uint8_t data[];
} bc_shm_ring_t;

typedef struct {
    int fd;
    void *base;
    bc_shm_ring_t *rx;
    bc_shm_ring_t *tx;
    size_t map_size;
} bc_shm_session_t;

int bc_shm_create(bc_shm_session_t *daemon_side);
int bc_shm_attach(int fd, bc_shm_session_t *session, int client_view);
void bc_shm_close(bc_shm_session_t *session);
int bc_shm_ring_write(bc_shm_ring_t *ring, const void *data, size_t len);
int bc_shm_ring_read(bc_shm_ring_t *ring, void *data, size_t cap, size_t *out_len);
int bc_shm_ring_peek_len(bc_shm_ring_t *ring, size_t *out_len);

#endif
