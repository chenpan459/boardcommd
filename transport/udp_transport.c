#include "transport.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    struct sockaddr_in remote;
} bc_udp_impl_t;

static int parse_endpoint(const char *endpoint, char *host, size_t host_len, int *port)
{
    const char *colon = strrchr(endpoint, ':');
    size_t len;

    if (colon == NULL) {
        return BC_ERR_INVALID;
    }

    len = (size_t)(colon - endpoint);
    if (len == 0 || len >= host_len) {
        return BC_ERR_INVALID;
    }

    memcpy(host, endpoint, len);
    host[len] = '\0';
    *port = atoi(colon + 1);
    return *port > 0 ? BC_OK : BC_ERR_INVALID;
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
    if (parse_endpoint(transport->config.endpoint, host, sizeof(host), &port) != BC_OK) {
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

    memset(&impl->remote, 0, sizeof(impl->remote));
    impl->remote.sin_family = AF_INET;
    impl->remote.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &impl->remote.sin_addr) != 1) {
        close(transport->fd);
        free(impl);
        return BC_ERR_INVALID;
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
    bc_udp_impl_t *impl = transport->impl;
    ssize_t n = sendto(
        transport->fd,
        data,
        len,
        0,
        (struct sockaddr *)&impl->remote,
        sizeof(impl->remote));

    return n == (ssize_t)len ? BC_OK : BC_ERR_IO;
}

static const bc_transport_ops_t udp_ops = {
    .open = udp_open,
    .close = udp_close,
    .send = udp_send,
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
