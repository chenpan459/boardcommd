#include "transport.h"

#include "endpoint.h"

#include "kcp/ikcp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define BC_KCP_RX_BUF_LEN 3200
#define BC_KCP_UDP_BUF_LEN 2048
#define BC_KCP_TIMER_MS 10

typedef struct {
    struct sockaddr_in remote;
    ikcpcb *kcp;
    int timer_fd;
    uint8_t rx_buf[BC_KCP_RX_BUF_LEN];
    size_t rx_len;
    uint32_t conv;
} bc_udp_kcp_impl_t;

static uint32_t kcp_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000U + (uint32_t)(ts.tv_nsec / 1000000U));
}

static bc_udp_kcp_impl_t *kcp_impl(bc_transport_t *transport)
{
    return transport != NULL ? (bc_udp_kcp_impl_t *)transport->impl : NULL;
}

static int kcp_udp_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
    bc_transport_t *transport = user;

    (void)kcp;
    bc_udp_kcp_impl_t *impl = kcp_impl(transport);

    if (impl == NULL || transport->fd < 0) {
        return -1;
    }

    if (sendto(
            transport->fd,
            buf,
            (size_t)len,
            0,
            (struct sockaddr *)&impl->remote,
            sizeof(impl->remote)) != len) {
        return -1;
    }
    return 0;
}

static uint32_t kcp_conv_from_config(const bc_transport_config_t *cfg)
{
    if (cfg->kcp_conv > 0) {
        return (uint32_t)cfg->kcp_conv;
    }
    if (cfg->local_port > 0) {
        return (uint32_t)cfg->local_port;
    }
    return 1U;
}

static int kcp_timer_start(int timer_fd)
{
    struct itimerspec ts;

    memset(&ts, 0, sizeof(ts));
    ts.it_value.tv_nsec = BC_KCP_TIMER_MS * 1000000L;
    ts.it_interval.tv_nsec = BC_KCP_TIMER_MS * 1000000L;
    return timerfd_settime(timer_fd, 0, &ts, NULL) == 0 ? BC_OK : BC_ERR_IO;
}

static int kcp_pump_udp(bc_transport_t *transport)
{
    bc_udp_kcp_impl_t *impl = kcp_impl(transport);
    uint8_t buf[BC_KCP_UDP_BUF_LEN];

    for (;;) {
        ssize_t n = recvfrom(
            transport->fd,
            buf,
            sizeof(buf),
            0,
            NULL,
            NULL);

        if (n > 0) {
            if (ikcp_input(impl->kcp, (const char *)buf, (long)n) < 0) {
                return BC_ERR_IO;
            }
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return BC_OK;
        }
        return BC_ERR_IO;
    }
}

static int kcp_drain_recv(bc_transport_t *transport)
{
    bc_udp_kcp_impl_t *impl = kcp_impl(transport);
    int n;

    for (;;) {
        if (impl->rx_len >= sizeof(impl->rx_buf)) {
            impl->rx_len = 0;
            return BC_ERR_IO;
        }

        n = ikcp_recv(
            impl->kcp,
            (char *)impl->rx_buf + impl->rx_len,
            (int)(sizeof(impl->rx_buf) - impl->rx_len));
        if (n > 0) {
            impl->rx_len += (size_t)n;
            continue;
        }
        if (n == -3) {
            return BC_OK;
        }
        return BC_ERR_IO;
    }
}

static void kcp_update_session(bc_transport_t *transport)
{
    bc_udp_kcp_impl_t *impl = kcp_impl(transport);
    uint32_t now = kcp_now_ms();

    ikcp_update(impl->kcp, now);
    (void)kcp_drain_recv(transport);
}

static int udp_kcp_open(bc_transport_t *transport)
{
    bc_udp_kcp_impl_t *impl = calloc(1, sizeof(*impl));
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
        transport->fd = -1;
        return BC_ERR_IO;
    }

    if (bc_fill_sockaddr_in(host, port, &impl->remote) != BC_OK) {
        close(transport->fd);
        free(impl);
        transport->fd = -1;
        return BC_ERR_INVALID;
    }

    impl->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (impl->timer_fd < 0) {
        close(transport->fd);
        free(impl);
        transport->fd = -1;
        return BC_ERR_IO;
    }
    if (kcp_timer_start(impl->timer_fd) != BC_OK) {
        close(impl->timer_fd);
        close(transport->fd);
        free(impl);
        transport->fd = -1;
        return BC_ERR_IO;
    }

    impl->conv = kcp_conv_from_config(&transport->config);
    impl->kcp = ikcp_create(impl->conv, transport);
    if (impl->kcp == NULL) {
        close(impl->timer_fd);
        close(transport->fd);
        free(impl);
        transport->fd = -1;
        return BC_ERR_NOMEM;
    }

    ikcp_setoutput(impl->kcp, kcp_udp_output);
    (void)ikcp_nodelay(impl->kcp, 1, BC_KCP_TIMER_MS, 2, 1);
    (void)ikcp_wndsize(impl->kcp, 256, 256);

    transport->impl = impl;
    return BC_OK;
}

static void udp_kcp_close(bc_transport_t *transport)
{
    bc_udp_kcp_impl_t *impl = kcp_impl(transport);

    if (impl == NULL) {
        return;
    }
    if (impl->kcp != NULL) {
        ikcp_release(impl->kcp);
        impl->kcp = NULL;
    }
    if (impl->timer_fd >= 0) {
        close(impl->timer_fd);
        impl->timer_fd = -1;
    }
    if (transport->fd >= 0) {
        close(transport->fd);
        transport->fd = -1;
    }
    free(impl);
    transport->impl = NULL;
}

static int udp_kcp_send(bc_transport_t *transport, const uint8_t *data, size_t len)
{
    bc_udp_kcp_impl_t *impl = kcp_impl(transport);

    if (impl == NULL || impl->kcp == NULL) {
        return BC_ERR_IO;
    }
    if (ikcp_send(impl->kcp, (const char *)data, (int)len) < 0) {
        return BC_ERR_IO;
    }
    kcp_update_session(transport);
    return BC_OK;
}

static int udp_kcp_recv(bc_transport_t *transport, uint8_t *buf, size_t cap, size_t *out_len)
{
    bc_udp_kcp_impl_t *impl = kcp_impl(transport);
    size_t n;

    if (impl == NULL || out_len == NULL) {
        return BC_ERR_INVALID;
    }

    kcp_update_session(transport);
    if (impl->rx_len == 0) {
        *out_len = 0;
        return BC_ERR_NOT_FOUND;
    }

    n = impl->rx_len < cap ? impl->rx_len : cap;
    memcpy(buf, impl->rx_buf, n);
    memmove(impl->rx_buf, impl->rx_buf + n, impl->rx_len - n);
    impl->rx_len -= n;
    *out_len = n;
    return BC_OK;
}

static int udp_kcp_get_fds(bc_transport_t *transport, int *fds, size_t *count)
{
    bc_udp_kcp_impl_t *impl = kcp_impl(transport);
    size_t n = 0;

    if (impl == NULL || fds == NULL || count == NULL) {
        return BC_ERR_INVALID;
    }
    if (transport->fd >= 0) {
        fds[n++] = transport->fd;
    }
    if (impl->timer_fd >= 0) {
        fds[n++] = impl->timer_fd;
    }
    *count = n;
    return BC_OK;
}

static int udp_kcp_handle_event(bc_transport_t *transport, int fd, uint32_t events)
{
    bc_udp_kcp_impl_t *impl = kcp_impl(transport);

    if (impl == NULL) {
        return BC_ERR_INVALID;
    }

    if (fd == impl->timer_fd && (events & EPOLLIN) != 0) {
        uint64_t ticks;

        while (read(impl->timer_fd, &ticks, sizeof(ticks)) == (ssize_t)sizeof(ticks)) {
            kcp_update_session(transport);
        }
        return BC_OK;
    }

    if (fd == transport->fd && (events & EPOLLIN) != 0) {
        if (kcp_pump_udp(transport) != BC_OK) {
            return BC_ERR_IO;
        }
        kcp_update_session(transport);
    }

    return BC_OK;
}

static const bc_transport_ops_t udp_kcp_ops = {
    .open = udp_kcp_open,
    .close = udp_kcp_close,
    .send = udp_kcp_send,
    .recv = udp_kcp_recv,
    .get_fds = udp_kcp_get_fds,
    .handle_event = udp_kcp_handle_event,
};

int bc_udp_kcp_transport_init(bc_transport_t *transport, const bc_transport_config_t *cfg)
{
    memset(transport, 0, sizeof(*transport));
    snprintf(transport->name, sizeof(transport->name), "%s", cfg->name);
    transport->type = BC_TRANSPORT_UDP_KCP;
    transport->fd = -1;
    transport->config = *cfg;
    transport->ops = &udp_kcp_ops;
    return BC_OK;
}
