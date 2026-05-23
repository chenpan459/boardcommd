#include "transport.h"

#include "endpoint.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    struct sockaddr_in remote;
} bc_udp_impl_t;

static bc_udp_impl_t *udp_impl(bc_transport_t *transport)
{
    return transport != NULL ? (bc_udp_impl_t *)transport->impl : NULL;
}

static int udp_open(bc_transport_t *transport)
{
    bc_udp_impl_t *impl = calloc(1, sizeof(*impl));
    struct sockaddr_in local;
    char host[64] = {0};
    int port = 0;

    if (impl == NULL) {
        return BC_ERR_NOMEM;
    }
    if (bc_parse_endpoint(transport->config.endpoint, host, sizeof(host), &port) != BC_OK) {
        free(impl);
        return BC_ERR_INVALID;
    }

    transport->fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (transport->fd < 0) {
        free(impl);
        return BC_ERR_IO;
    }

    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons((uint16_t)transport->config.local_port);
    if (bind(transport->fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(transport->fd);
        free(impl);
        return BC_ERR_IO;
    }

    if (bc_fill_sockaddr_in(host, port, &impl->remote) != BC_OK) {
        close(transport->fd);
        free(impl);
        return BC_ERR_IO;
    }

    transport->impl = impl;
    return BC_OK;
}

static void udp_close(bc_transport_t *transport)
{
    if (transport->fd >= 0) {
        close(transport->fd);
        transport->fd = -1;
    }
    free(transport->impl);
    transport->impl = NULL;
}

static int udp_send(bc_transport_t *transport, const uint8_t *data, size_t len)
{
    bc_udp_impl_t *impl = udp_impl(transport);
    ssize_t n = sendto(
        transport->fd,
        data,
        len,
        0,
        (struct sockaddr *)&impl->remote,
        sizeof(impl->remote));

    if (n == (ssize_t)len) {
        return BC_OK;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return BC_ERR_NOMEM;
    }
    return BC_ERR_IO;
}

static int udp_recv(bc_transport_t *transport, uint8_t *buf, size_t cap, size_t *out_len)
{
    ssize_t n = recvfrom(transport->fd, buf, cap, 0, NULL, NULL);

    if (n > 0) {
        *out_len = (size_t)n;
        return BC_OK;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        *out_len = 0;
        return BC_ERR_NOT_FOUND;
    }
    return BC_ERR_IO;
}

static const bc_transport_ops_t udp_ops = {
    .open = udp_open,
    .close = udp_close,
    .send = udp_send,
    .recv = udp_recv,
};

int bc_udp_transport_init(bc_transport_t *transport, const bc_transport_config_t *cfg)
{
    memset(transport, 0, sizeof(*transport));
    snprintf(transport->name, sizeof(transport->name), "%s", cfg->name);
    transport->type = BC_TRANSPORT_UDP;
    transport->fd = -1;
    transport->config = *cfg;
    transport->ops = &udp_ops;
    return BC_OK;
}
