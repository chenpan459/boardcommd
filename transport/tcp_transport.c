#include "transport.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

static int tcp_open(bc_transport_t *transport)
{
    struct sockaddr_in remote;
    char host[64] = {0};
    int port = 0;

    if (parse_endpoint(transport->config.endpoint, host, sizeof(host), &port) != BC_OK) {
        return BC_ERR_INVALID;
    }

    transport->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (transport->fd < 0) {
        return BC_ERR_IO;
    }

    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &remote.sin_addr) != 1) {
        close(transport->fd);
        transport->fd = -1;
        return BC_ERR_INVALID;
    }

    (void)connect(transport->fd, (struct sockaddr *)&remote, sizeof(remote));
    return BC_OK;
}

static void tcp_close(bc_transport_t *transport)
{
    if (transport->fd >= 0) {
        close(transport->fd);
        transport->fd = -1;
    }
}

static int tcp_send(bc_transport_t *transport, const uint8_t *data, size_t len)
{
    ssize_t n = send(transport->fd, data, len, MSG_NOSIGNAL);

    return n == (ssize_t)len ? BC_OK : BC_ERR_IO;
}

static const bc_transport_ops_t tcp_ops = {
    .open = tcp_open,
    .close = tcp_close,
    .send = tcp_send,
};

int bc_tcp_transport_init(bc_transport_t *transport, const bc_transport_config_t *cfg)
{
    memset(transport, 0, sizeof(*transport));
    snprintf(transport->name, sizeof(transport->name), "%s", cfg->name);
    transport->type = BC_TRANSPORT_TCP;
    transport->fd = -1;
    transport->config = *cfg;
    transport->ops = &tcp_ops;
    return BC_OK;
}
