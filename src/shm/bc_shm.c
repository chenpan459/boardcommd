#include "bc_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static size_t shm_map_size(void)
{
    return sizeof(bc_shm_ring_t) + BC_SHM_RING_SIZE + sizeof(bc_shm_ring_t) + BC_SHM_RING_SIZE;
}

static int init_ring(bc_shm_ring_t *ring, uint32_t size)
{
    memset(ring, 0, sizeof(*ring));
    ring->size = size;
    return BC_OK;
}

static int map_session(int fd, bc_shm_session_t *session, int client_view)
{
    void *base;

    session->fd = fd;
    session->map_size = shm_map_size();
    base = mmap(NULL, session->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        session->fd = -1;
        return BC_ERR_IO;
    }

    session->base = base;
    if (client_view) {
        session->tx = (bc_shm_ring_t *)base;
        session->rx = (bc_shm_ring_t *)((uint8_t *)base + sizeof(bc_shm_ring_t) + BC_SHM_RING_SIZE);
    } else {
        session->rx = (bc_shm_ring_t *)base;
        session->tx = (bc_shm_ring_t *)((uint8_t *)base + sizeof(bc_shm_ring_t) + BC_SHM_RING_SIZE);
    }
    return BC_OK;
}

int bc_shm_attach(int fd, bc_shm_session_t *session, int client_view)
{
    memset(session, 0, sizeof(*session));
    return map_session(fd, session, client_view);
}

int bc_shm_create(bc_shm_session_t *daemon_side)
{
    int fd;
    void *base;
    size_t map_size = shm_map_size();
    bc_shm_ring_t *ring_a;
    bc_shm_ring_t *ring_b;

    if (daemon_side == NULL) {
        return BC_ERR_INVALID;
    }

    fd = memfd_create("bc_shm", MFD_CLOEXEC);
    if (fd < 0) {
        return BC_ERR_IO;
    }
    if (ftruncate(fd, (off_t)map_size) != 0) {
        close(fd);
        return BC_ERR_IO;
    }

    base = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        return BC_ERR_IO;
    }

    ring_a = (bc_shm_ring_t *)base;
    ring_b = (bc_shm_ring_t *)((uint8_t *)base + sizeof(bc_shm_ring_t) + BC_SHM_RING_SIZE);
    (void)init_ring(ring_a, BC_SHM_RING_SIZE);
    (void)init_ring(ring_b, BC_SHM_RING_SIZE);
    munmap(base, map_size);

    memset(daemon_side, 0, sizeof(*daemon_side));
    if (map_session(fd, daemon_side, 0) != BC_OK) {
        close(fd);
        return BC_ERR_IO;
    }
    return BC_OK;
}

void bc_shm_close(bc_shm_session_t *session)
{
    if (session == NULL) {
        return;
    }
    if (session->base != NULL) {
        munmap(session->base, session->map_size);
    }
    if (session->fd >= 0) {
        close(session->fd);
    }
    memset(session, 0, sizeof(*session));
    session->fd = -1;
}

static uint32_t ring_space(const bc_shm_ring_t *ring)
{
    return ring->size - (ring->head - ring->tail);
}

static uint32_t ring_data(const bc_shm_ring_t *ring)
{
    return ring->head - ring->tail;
}

int bc_shm_ring_write(bc_shm_ring_t *ring, const void *data, size_t len)
{
    const uint8_t *src = data;
    uint32_t space;
    uint32_t head;
    uint32_t i;

    if (ring == NULL || (len > 0 && data == NULL)) {
        return BC_ERR_INVALID;
    }
    if (len > BC_SHM_RING_SIZE - sizeof(uint32_t)) {
        return BC_ERR_INVALID;
    }

    space = ring_space(ring);
    if (space < sizeof(uint32_t) + len) {
        return BC_ERR_NOMEM;
    }

    head = ring->head % ring->size;
    for (i = 0; i < sizeof(uint32_t); ++i) {
        ring->data[(head + i) % ring->size] = ((const uint8_t *)&len)[i];
    }
    head = (head + (uint32_t)sizeof(uint32_t)) % ring->size;
    for (i = 0; i < len; ++i) {
        ring->data[(head + i) % ring->size] = src[i];
    }
    ring->head += (uint32_t)sizeof(uint32_t) + (uint32_t)len;
    return BC_OK;
}

int bc_shm_ring_peek_len(bc_shm_ring_t *ring, size_t *out_len)
{
    uint32_t avail;
    uint32_t tail;
    uint32_t len = 0;
    uint32_t i;

    if (ring == NULL || out_len == NULL) {
        return BC_ERR_INVALID;
    }

    avail = ring_data(ring);
    if (avail < sizeof(uint32_t)) {
        return BC_ERR_NOT_FOUND;
    }

    tail = ring->tail % ring->size;
    for (i = 0; i < sizeof(uint32_t); ++i) {
        ((uint8_t *)&len)[i] = ring->data[(tail + i) % ring->size];
    }
    if (avail < sizeof(uint32_t) + len) {
        return BC_ERR_NOT_FOUND;
    }
    *out_len = len;
    return BC_OK;
}

int bc_shm_ring_read(bc_shm_ring_t *ring, void *data, size_t cap, size_t *out_len)
{
    size_t len = 0;
    uint32_t tail;
    size_t i;
    int rc;

    if (out_len == NULL) {
        return BC_ERR_INVALID;
    }

    rc = bc_shm_ring_peek_len(ring, &len);
    if (rc != BC_OK) {
        return rc;
    }
    if (len > cap) {
        return BC_ERR_INVALID;
    }

    tail = (ring->tail + (uint32_t)sizeof(uint32_t)) % ring->size;
    for (i = 0; i < len; ++i) {
        ((uint8_t *)data)[i] = ring->data[(tail + (uint32_t)i) % ring->size];
    }
    ring->tail += (uint32_t)sizeof(uint32_t) + (uint32_t)len;
    *out_len = len;
    return BC_OK;
}
