#include "transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int listen_fd;
    int connected;
    int connecting;
    char host[64];
    int port;
} bc_tcp_impl_t;

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

static bc_tcp_impl_t *tcp_impl(bc_transport_t *transport)
{
    return transport != NULL ? (bc_tcp_impl_t *)transport->impl : NULL;
}

static int tcp_set_connected_fd(bc_transport_t *transport, int fd)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);

    if (transport->fd >= 0 && transport->fd != impl->listen_fd) {
        close(transport->fd);
    }
    transport->fd = fd;
    impl->connected = 1;
    impl->connecting = 0;
    return BC_OK;
}

static int tcp_listen_open(bc_transport_t *transport, bc_tcp_impl_t *impl)
{
    struct sockaddr_in addr;
    int reuse = 1;

    impl->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (impl->listen_fd < 0) {
        return BC_ERR_IO;
    }

    (void)setsockopt(impl->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)impl->port);
    if (impl->host[0] == '\0' || strcmp(impl->host, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, impl->host, &addr.sin_addr) != 1) {
        close(impl->listen_fd);
        impl->listen_fd = -1;
        return BC_ERR_INVALID;
    }

    if (bind(impl->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(impl->listen_fd, 8) < 0) {
        close(impl->listen_fd);
        impl->listen_fd = -1;
        return BC_ERR_IO;
    }

    transport->fd = impl->listen_fd;
    return BC_OK;
}

static int tcp_client_open(bc_transport_t *transport, bc_tcp_impl_t *impl)
{
    struct sockaddr_in remote;

    transport->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (transport->fd < 0) {
        return BC_ERR_IO;
    }

    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = htons((uint16_t)impl->port);
    if (inet_pton(AF_INET, impl->host, &remote.sin_addr) != 1) {
        close(transport->fd);
        transport->fd = -1;
        return BC_ERR_INVALID;
    }

    if (connect(transport->fd, (struct sockaddr *)&remote, sizeof(remote)) < 0) {
        if (errno == EINPROGRESS) {
            impl->connecting = 1;
            return BC_OK;
        }
        close(transport->fd);
        transport->fd = -1;
        return BC_ERR_IO;
    }

    impl->connected = 1;
    impl->connecting = 0;
    return BC_OK;
}

static int tcp_role_is_server(const bc_transport_config_t *cfg)
{
    if (cfg->tcp_listen >= 0) {
        return cfg->tcp_listen != 0;
    }
    return cfg->local_port > 0;
}

static int tcp_listen_port(const bc_transport_config_t *cfg, int endpoint_port)
{
    if (cfg->local_port > 0) {
        return cfg->local_port;
    }
    return endpoint_port;
}

static int tcp_open(bc_transport_t *transport)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);
    const bc_transport_config_t *cfg = &transport->config;
    int endpoint_port = 0;

    if (impl == NULL) {
        return BC_ERR_INVALID;
    }

    if (tcp_role_is_server(cfg)) {
        if (parse_endpoint(cfg->endpoint, impl->host, sizeof(impl->host), &endpoint_port) != BC_OK) {
            snprintf(impl->host, sizeof(impl->host), "0.0.0.0");
            endpoint_port = 0;
        }
        impl->port = tcp_listen_port(cfg, endpoint_port);
        if (impl->port <= 0) {
            return BC_ERR_INVALID;
        }
        return tcp_listen_open(transport, impl);
    }

    if (parse_endpoint(cfg->endpoint, impl->host, sizeof(impl->host), &impl->port) != BC_OK) {
        return BC_ERR_INVALID;
    }
    return tcp_client_open(transport, impl);
}

static void tcp_close(bc_transport_t *transport)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);

    if (impl == NULL) {
        return;
    }
    if (transport->fd >= 0 && transport->fd != impl->listen_fd) {
        close(transport->fd);
    }
    if (impl->listen_fd >= 0) {
        close(impl->listen_fd);
        impl->listen_fd = -1;
    }
    transport->fd = -1;
    impl->connected = 0;
    impl->connecting = 0;
    free(impl);
    transport->impl = NULL;
}

static int tcp_send(bc_transport_t *transport, const uint8_t *data, size_t len)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);
    size_t off = 0;

    if (impl == NULL || !impl->connected || transport->fd < 0) {
        return BC_ERR_IO;
    }

    while (off < len) {
        ssize_t n = send(transport->fd, data + off, len - off, MSG_NOSIGNAL);

        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return BC_ERR_IO;
        }
        impl->connected = 0;
        return BC_ERR_IO;
    }
    return BC_OK;
}

static int tcp_get_fds(bc_transport_t *transport, int *fds, size_t *count)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);
    size_t n = 0;

    if (impl == NULL || fds == NULL || count == NULL) {
        return BC_ERR_INVALID;
    }

    if (impl->listen_fd >= 0) {
        fds[n++] = impl->listen_fd;
    }
    if (transport->fd >= 0 && transport->fd != impl->listen_fd) {
        fds[n++] = transport->fd;
    } else if (impl->connecting && transport->fd >= 0) {
        fds[n++] = transport->fd;
    }

    *count = n;
    return BC_OK;
}

static int tcp_handle_event(bc_transport_t *transport, int fd, uint32_t events)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);

    if (impl == NULL) {
        return BC_ERR_INVALID;
    }

    if (fd == impl->listen_fd && (events & EPOLLIN) != 0) {
        for (;;) {
            int client_fd = accept4(impl->listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);

            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            (void)tcp_set_connected_fd(transport, client_fd);
            return BC_OK;
        }
    }

    if (impl->connecting && fd == transport->fd && (events & (EPOLLOUT | EPOLLIN | EPOLLERR)) != 0) {
        int err = 0;
        socklen_t err_len = sizeof(err);

        if (getsockopt(transport->fd, SOL_SOCKET, SO_ERROR, &err, &err_len) == 0 && err == 0) {
            impl->connected = 1;
            impl->connecting = 0;
            return BC_OK;
        }
        impl->connecting = 0;
        close(transport->fd);
        transport->fd = -1;
        return BC_ERR_IO;
    }

    if (fd == transport->fd && (events & (EPOLLHUP | EPOLLERR)) != 0) {
        impl->connected = 0;
        if (impl->listen_fd >= 0) {
            close(transport->fd);
            transport->fd = impl->listen_fd;
        }
    }

    return BC_OK;
}

static const bc_transport_ops_t tcp_ops = {
    .open = tcp_open,
    .close = tcp_close,
    .send = tcp_send,
    .get_fds = tcp_get_fds,
    .handle_event = tcp_handle_event,
};

int bc_tcp_transport_init(bc_transport_t *transport, const bc_transport_config_t *cfg)
{
    bc_tcp_impl_t *impl;

    memset(transport, 0, sizeof(*transport));
    snprintf(transport->name, sizeof(transport->name), "%s", cfg->name);
    transport->type = BC_TRANSPORT_TCP;
    transport->fd = -1;
    transport->config = *cfg;
    impl = calloc(1, sizeof(*impl));
    if (impl == NULL) {
        return BC_ERR_NOMEM;
    }
    impl->listen_fd = -1;
    transport->impl = impl;
    transport->ops = &tcp_ops;
    return BC_OK;
}
