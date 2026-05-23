#include "transport.h"

#include "endpoint.h"
#include "protocol.h"
#include "reactor.h"
#include "bc_log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define BC_TCP_TAG "tcp"
#define BC_TCP_SEND_CAP (BC_MAX_FRAME_LEN * 8)
#define BC_TCP_RECONNECT_MS_INIT 1000
#define BC_TCP_RECONNECT_MS_MAX 30000
#define BC_TCP_KEEPALIVE_IDLE 30
#define BC_TCP_KEEPALIVE_INTVL 5
#define BC_TCP_KEEPALIVE_CNT 3

typedef struct {
    int listen_fd;
    int conn_fd;
    int timer_fd;
    int is_server;
    int connected;
    int connecting;
    int reconnect_armed;
    char host[64];
    int port;
    char peer[64];
    int retry_attempt;
    uint8_t *send_buf;
    size_t send_len;
    size_t send_off;
    bc_reactor_t *reactor;
    bc_reactor_cb event_cb;
    void *event_user;
} bc_tcp_impl_t;

static bc_tcp_impl_t *tcp_impl(bc_transport_t *transport)
{
    return transport != NULL ? (bc_tcp_impl_t *)transport->impl : NULL;
}

static int tcp_active_fd(bc_transport_t *transport)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);

    if (impl == NULL) {
        return -1;
    }
    if (impl->connected && impl->conn_fd >= 0) {
        return impl->conn_fd;
    }
    if (impl->connecting && impl->conn_fd >= 0) {
        return impl->conn_fd;
    }
    if (impl->is_server && impl->listen_fd >= 0) {
        return impl->listen_fd;
    }
    return impl->conn_fd;
}

static void tcp_sync_transport_fd(bc_transport_t *transport)
{
    transport->fd = tcp_active_fd(transport);
}

static uint32_t tcp_conn_events(const bc_tcp_impl_t *impl)
{
    uint32_t events = EPOLLIN | EPOLLHUP | EPOLLERR;

    if (impl->connecting || impl->send_off < impl->send_len) {
        events |= EPOLLOUT;
    }
    return events;
}

static int tcp_apply_socket_opts(int fd)
{
    int on = 1;
    int sndbuf = 32 * 1024;
    int rcvbuf = 32 * 1024;

    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    int keepidle = BC_TCP_KEEPALIVE_IDLE;
    int keepintvl = BC_TCP_KEEPALIVE_INTVL;
    int keepcnt = BC_TCP_KEEPALIVE_CNT;

    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
#ifdef TCP_KEEPIDLE
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
#endif
    return BC_OK;
}

static int tcp_watch_fd(bc_transport_t *transport, int fd, uint32_t events)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);

    if (impl == NULL || impl->reactor == NULL || fd < 0) {
        return BC_OK;
    }
    if (bc_reactor_mod(impl->reactor, fd, events, impl->event_cb, impl->event_user) == BC_OK) {
        return BC_OK;
    }
    return bc_reactor_add(impl->reactor, fd, events, impl->event_cb, impl->event_user);
}

static void tcp_unwatch_fd(bc_transport_t *transport, int fd)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);

    if (impl != NULL && impl->reactor != NULL && fd >= 0) {
        bc_reactor_del(impl->reactor, fd);
    }
}

static void tcp_clear_send_queue(bc_tcp_impl_t *impl)
{
    impl->send_len = 0;
    impl->send_off = 0;
}

static int tcp_ensure_send_cap(bc_tcp_impl_t *impl, size_t need)
{
    if (need > BC_TCP_SEND_CAP) {
        return BC_ERR_NOMEM;
    }
    if (impl->send_buf == NULL) {
        impl->send_buf = malloc(BC_TCP_SEND_CAP);
        if (impl->send_buf == NULL) {
            return BC_ERR_NOMEM;
        }
    }
    return BC_OK;
}

static int tcp_enable_epollout(bc_transport_t *transport)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);

    if (impl == NULL || impl->conn_fd < 0 || !impl->connected) {
        return BC_OK;
    }
    return tcp_watch_fd(transport, impl->conn_fd, tcp_conn_events(impl));
}

static int tcp_flush_send(bc_transport_t *transport)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);

    if (impl == NULL || !impl->connected || impl->conn_fd < 0) {
        return BC_ERR_IO;
    }

    while (impl->send_off < impl->send_len) {
        ssize_t n = send(
            impl->conn_fd,
            impl->send_buf + impl->send_off,
            impl->send_len - impl->send_off,
            MSG_NOSIGNAL);

        if (n > 0) {
            impl->send_off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return tcp_enable_epollout(transport);
        }
        return BC_ERR_IO;
    }

    tcp_clear_send_queue(impl);
    return tcp_watch_fd(transport, impl->conn_fd, tcp_conn_events(impl));
}

static int tcp_queue_send(bc_transport_t *transport, const uint8_t *data, size_t len)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);
    size_t need;

    if (impl == NULL) {
        return BC_ERR_IO;
    }

    need = (impl->send_len - impl->send_off) + len;
    if (tcp_ensure_send_cap(impl, need) != BC_OK) {
        return BC_ERR_NOMEM;
    }

    memcpy(impl->send_buf + impl->send_len, data, len);
    impl->send_len += len;
    (void)tcp_enable_epollout(transport);
    return BC_OK;
}

static int tcp_reconnect_delay_ms(bc_tcp_impl_t *impl)
{
    int delay = BC_TCP_RECONNECT_MS_INIT;

    if (impl->retry_attempt > 0) {
        delay = BC_TCP_RECONNECT_MS_INIT << (impl->retry_attempt - 1);
        if (delay > BC_TCP_RECONNECT_MS_MAX) {
            delay = BC_TCP_RECONNECT_MS_MAX;
        }
    }
    return delay;
}

static int tcp_timer_arm(bc_transport_t *transport, int delay_ms)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);
    struct itimerspec ts;

    if (impl == NULL || impl->timer_fd < 0) {
        return BC_ERR_IO;
    }

    memset(&ts, 0, sizeof(ts));
    ts.it_value.tv_sec = (time_t)(delay_ms / 1000);
    ts.it_value.tv_nsec = (long)(delay_ms % 1000) * 1000000L;
    return timerfd_settime(impl->timer_fd, 0, &ts, NULL) == 0 ? BC_OK : BC_ERR_IO;
}

static void tcp_schedule_reconnect(bc_transport_t *transport)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);
    int delay;

    if (impl == NULL || impl->is_server) {
        return;
    }

    impl->reconnect_armed = 1;
    delay = tcp_reconnect_delay_ms(impl);
    impl->retry_attempt++;
    (void)tcp_timer_arm(transport, delay);
    BC_LOGW(BC_TCP_TAG, "%s: reconnect in %d ms (attempt %d)", transport->name, delay, impl->retry_attempt);
}

static void tcp_close_conn(bc_transport_t *transport)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);

    if (impl == NULL) {
        return;
    }

    if (impl->conn_fd >= 0) {
        tcp_unwatch_fd(transport, impl->conn_fd);
        close(impl->conn_fd);
        impl->conn_fd = -1;
    }
    impl->connected = 0;
    impl->connecting = 0;
    impl->peer[0] = '\0';
    tcp_clear_send_queue(impl);
    tcp_sync_transport_fd(transport);
}

static void tcp_drop_connection(bc_transport_t *transport, const char *reason)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);

    if (impl == NULL || (!impl->connected && !impl->connecting)) {
        return;
    }

    BC_LOGW(BC_TCP_TAG, "%s: disconnected (%s) peer=%s", transport->name, reason, impl->peer[0] ? impl->peer : "-");
    tcp_close_conn(transport);
    if (!impl->is_server) {
        tcp_schedule_reconnect(transport);
    }
}

static int tcp_begin_connect(bc_transport_t *transport)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);
    struct sockaddr_in remote;

    if (impl->conn_fd >= 0) {
        tcp_unwatch_fd(transport, impl->conn_fd);
        close(impl->conn_fd);
        impl->conn_fd = -1;
    }

    impl->conn_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (impl->conn_fd < 0) {
        return BC_ERR_IO;
    }

    (void)tcp_apply_socket_opts(impl->conn_fd);
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = htons((uint16_t)impl->port);
    if (inet_pton(AF_INET, impl->host, &remote.sin_addr) != 1) {
        close(impl->conn_fd);
        impl->conn_fd = -1;
        return BC_ERR_INVALID;
    }

    impl->connecting = 1;
    impl->connected = 0;
    impl->reconnect_armed = 0;
    tcp_sync_transport_fd(transport);

    if (connect(impl->conn_fd, (struct sockaddr *)&remote, sizeof(remote)) < 0) {
        if (errno == EINPROGRESS) {
            return tcp_watch_fd(transport, impl->conn_fd, tcp_conn_events(impl));
        }
        tcp_close_conn(transport);
        return BC_ERR_IO;
    }

    impl->connecting = 0;
    impl->connected = 1;
    BC_LOGI(BC_TCP_TAG, "%s: connected to %s:%d", transport->name, impl->host, impl->port);
    return tcp_watch_fd(transport, impl->conn_fd, tcp_conn_events(impl));
}

static int tcp_on_connect_ready(bc_transport_t *transport)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);
    int err = 0;
    socklen_t err_len = sizeof(err);

    if (getsockopt(impl->conn_fd, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0 || err != 0) {
        tcp_drop_connection(transport, "connect failed");
        if (!impl->is_server) {
            tcp_schedule_reconnect(transport);
        }
        return BC_ERR_IO;
    }

    impl->connecting = 0;
    impl->connected = 1;
    impl->retry_attempt = 0;
    snprintf(impl->peer, sizeof(impl->peer), "%s:%d", impl->host, impl->port);
    BC_LOGI(BC_TCP_TAG, "%s: connected to %s", transport->name, impl->peer);
    return tcp_watch_fd(transport, impl->conn_fd, tcp_conn_events(impl));
}

static int tcp_accept_one(bc_transport_t *transport)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    char peer_host[INET_ADDRSTRLEN];
    int client_fd;

    client_fd = accept4(impl->listen_fd, (struct sockaddr *)&peer, &peer_len, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client_fd < 0) {
        return errno == EINTR ? BC_OK : BC_ERR_IO;
    }

    if (impl->connected && impl->conn_fd >= 0) {
        BC_LOGW(BC_TCP_TAG, "%s: replacing existing connection", transport->name);
        tcp_drop_connection(transport, "replaced by new client");
    }

    (void)tcp_apply_socket_opts(client_fd);
    impl->conn_fd = client_fd;
    impl->connecting = 0;
    impl->connected = 1;
    impl->retry_attempt = 0;
    if (inet_ntop(AF_INET, &peer.sin_addr, peer_host, sizeof(peer_host)) != NULL) {
        snprintf(impl->peer, sizeof(impl->peer), "%s:%u", peer_host, ntohs(peer.sin_port));
    } else {
        snprintf(impl->peer, sizeof(impl->peer), "unknown:%u", ntohs(peer.sin_port));
    }

    BC_LOGI(BC_TCP_TAG, "%s: accepted %s", transport->name, impl->peer);
    tcp_sync_transport_fd(transport);
    return tcp_watch_fd(transport, impl->conn_fd, tcp_conn_events(impl));
}

static int tcp_listen_open(bc_transport_t *transport, bc_tcp_impl_t *impl)
{
    struct sockaddr_in addr;
    int reuse = 1;

    impl->is_server = 1;
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
        listen(impl->listen_fd, 16) < 0) {
        close(impl->listen_fd);
        impl->listen_fd = -1;
        return BC_ERR_IO;
    }

    tcp_sync_transport_fd(transport);
    BC_LOGI(BC_TCP_TAG, "%s: listening on %s:%d", transport->name, impl->host, impl->port);
    return BC_OK;
}

static int tcp_client_setup(bc_transport_t *transport, bc_tcp_impl_t *impl)
{
    impl->is_server = 0;
    impl->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (impl->timer_fd < 0) {
        return BC_ERR_IO;
    }
    if (tcp_begin_connect(transport) != BC_OK) {
        tcp_schedule_reconnect(transport);
    }
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

    impl->listen_fd = -1;
    impl->conn_fd = -1;
    impl->timer_fd = -1;

    if (tcp_role_is_server(cfg)) {
        if (bc_parse_endpoint(cfg->endpoint, impl->host, sizeof(impl->host), &endpoint_port) != BC_OK) {
            snprintf(impl->host, sizeof(impl->host), "0.0.0.0");
            endpoint_port = 0;
        }
        impl->port = tcp_listen_port(cfg, endpoint_port);
        if (impl->port <= 0) {
            return BC_ERR_INVALID;
        }
        return tcp_listen_open(transport, impl);
    }

    if (bc_parse_endpoint(cfg->endpoint, impl->host, sizeof(impl->host), &impl->port) != BC_OK) {
        return BC_ERR_INVALID;
    }
    return tcp_client_setup(transport, impl);
}

static void tcp_close(bc_transport_t *transport)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);

    if (impl == NULL) {
        return;
    }

    if (impl->conn_fd >= 0) {
        tcp_unwatch_fd(transport, impl->conn_fd);
        close(impl->conn_fd);
        impl->conn_fd = -1;
    }
    if (impl->listen_fd >= 0) {
        tcp_unwatch_fd(transport, impl->listen_fd);
        close(impl->listen_fd);
        impl->listen_fd = -1;
    }
    if (impl->timer_fd >= 0) {
        tcp_unwatch_fd(transport, impl->timer_fd);
        close(impl->timer_fd);
        impl->timer_fd = -1;
    }

    free(impl->send_buf);
    impl->send_buf = NULL;
    transport->fd = -1;
    free(impl);
    transport->impl = NULL;
}

static int tcp_send(bc_transport_t *transport, const uint8_t *data, size_t len)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);
    size_t off = 0;

    if (impl == NULL || !impl->connected || impl->conn_fd < 0) {
        return BC_ERR_IO;
    }

    if (impl->send_off < impl->send_len) {
        return tcp_queue_send(transport, data, len);
    }

    while (off < len) {
        ssize_t n = send(impl->conn_fd, data + off, len - off, MSG_NOSIGNAL);

        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return tcp_queue_send(transport, data + off, len - off);
        }
        tcp_drop_connection(transport, "send error");
        return BC_ERR_IO;
    }

    return BC_OK;
}

static int tcp_recv(bc_transport_t *transport, uint8_t *buf, size_t cap, size_t *out_len)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);
    ssize_t n;

    if (impl == NULL || !impl->connected || impl->conn_fd < 0 || out_len == NULL) {
        return BC_ERR_INVALID;
    }

    n = recv(impl->conn_fd, buf, cap, 0);
    if (n > 0) {
        *out_len = (size_t)n;
        return BC_OK;
    }
    if (n == 0) {
        tcp_drop_connection(transport, "peer closed");
        return BC_ERR_IO;
    }
    if (errno == EINTR) {
        *out_len = 0;
        return BC_ERR_NOT_FOUND;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        *out_len = 0;
        return BC_ERR_NOT_FOUND;
    }

    tcp_drop_connection(transport, "recv error");
    return BC_ERR_IO;
}

static int tcp_bind_reactor(bc_transport_t *transport, bc_reactor_t *reactor, bc_reactor_cb cb, void *user)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);

    if (impl == NULL) {
        return BC_ERR_INVALID;
    }

    impl->reactor = reactor;
    impl->event_cb = cb;
    impl->event_user = user;

    if (impl->listen_fd >= 0) {
        if (tcp_watch_fd(transport, impl->listen_fd, EPOLLIN) != BC_OK) {
            return BC_ERR_IO;
        }
    }
    if (impl->conn_fd >= 0) {
        if (tcp_watch_fd(transport, impl->conn_fd, tcp_conn_events(impl)) != BC_OK) {
            return BC_ERR_IO;
        }
    }
    if (impl->timer_fd >= 0) {
        if (tcp_watch_fd(transport, impl->timer_fd, EPOLLIN) != BC_OK) {
            return BC_ERR_IO;
        }
    }
    return BC_OK;
}

static int tcp_get_fds(bc_transport_t *transport, int *fds, size_t *count)
{
    (void)transport;
    if (count != NULL) {
        *count = 0;
    }
    (void)fds;
    return BC_OK;
}

static int tcp_handle_event(bc_transport_t *transport, int fd, uint32_t events)
{
    bc_tcp_impl_t *impl = tcp_impl(transport);

    if (impl == NULL) {
        return BC_ERR_INVALID;
    }

    if (fd == impl->timer_fd && (events & EPOLLIN) != 0) {
        uint64_t expirations;

        while (read(impl->timer_fd, &expirations, sizeof(expirations)) == (ssize_t)sizeof(expirations)) {
            if (impl->reconnect_armed && !impl->connected && !impl->connecting) {
                impl->reconnect_armed = 0;
                (void)tcp_begin_connect(transport);
            }
        }
        return BC_OK;
    }

    if (fd == impl->listen_fd && (events & EPOLLIN) != 0) {
        for (;;) {
            if (tcp_accept_one(transport) != BC_OK) {
                break;
            }
        }
        return BC_OK;
    }

    if (fd == impl->conn_fd && impl->connecting && (events & (EPOLLOUT | EPOLLIN | EPOLLERR)) != 0) {
        return tcp_on_connect_ready(transport);
    }

    if (fd == impl->conn_fd && impl->connected) {
        if ((events & (EPOLLHUP | EPOLLERR)) != 0) {
            tcp_drop_connection(transport, "hangup");
            return BC_OK;
        }
        if ((events & EPOLLOUT) != 0 && impl->send_off < impl->send_len) {
            if (tcp_flush_send(transport) != BC_OK) {
                tcp_drop_connection(transport, "flush failed");
            }
        }
    }

    return BC_OK;
}

static const bc_transport_ops_t tcp_ops = {
    .open = tcp_open,
    .close = tcp_close,
    .send = tcp_send,
    .recv = tcp_recv,
    .get_fds = tcp_get_fds,
    .bind_reactor = tcp_bind_reactor,
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
    impl->conn_fd = -1;
    impl->timer_fd = -1;
    transport->impl = impl;
    transport->ops = &tcp_ops;
    return BC_OK;
}
