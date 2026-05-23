#include "client_manager.h"
#include "boardcomm_version.h"
#include "config.h"
#include "message_bus.h"
#include "protocol.h"
#include "reactor.h"
#include "router.h"
#include "transport_manager.h"
#include "bc_log.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

typedef struct {
    bc_transport_t *transport;
    bc_message_bus_t *bus;
    uint8_t rx_buf[BC_MAX_FRAME_LEN * 2];
    size_t rx_len;
} bc_transport_event_ctx_t;

static bc_reactor_t *g_reactor;

static void on_signal(int sig)
{
    (void)sig;
    if (g_reactor != NULL) {
        bc_reactor_stop(g_reactor);
    }
}

static int process_transport_frames(bc_transport_event_ctx_t *ctx)
{
    for (;;) {
        size_t frame_len = 0;
        bc_message_t msg;
        int rc = bc_protocol_frame_length(ctx->rx_buf, ctx->rx_len, &frame_len);

        if (rc == BC_ERR_NOT_FOUND) {
            return BC_OK;
        }
        if (rc != BC_OK || frame_len > ctx->rx_len) {
            return BC_ERR_INVALID;
        }
        if (bc_protocol_decode(ctx->rx_buf, frame_len, &msg) != BC_OK) {
            return BC_ERR_INVALID;
        }

        (void)bc_message_bus_handle_inbound(ctx->bus, &msg, ctx->transport);
        memmove(ctx->rx_buf, ctx->rx_buf + frame_len, ctx->rx_len - frame_len);
        ctx->rx_len -= frame_len;
    }
}

static void on_transport_event(int fd, uint32_t events, void *user)
{
    bc_transport_event_ctx_t *ctx = user;

    if (ctx->transport->ops != NULL && ctx->transport->ops->handle_event != NULL) {
        (void)ctx->transport->ops->handle_event(ctx->transport, fd, events);
    }

    for (;;) {
        ssize_t n;

        if (ctx->rx_len >= sizeof(ctx->rx_buf)) {
            ctx->rx_len = 0;
            return;
        }

        if (ctx->transport->ops != NULL && ctx->transport->ops->recv != NULL) {
            size_t got = 0;
            int rc = ctx->transport->ops->recv(
                ctx->transport,
                ctx->rx_buf + ctx->rx_len,
                sizeof(ctx->rx_buf) - ctx->rx_len,
                &got);

            if (rc == BC_OK && got > 0) {
                n = (ssize_t)got;
            } else if (rc == BC_ERR_NOT_FOUND) {
                return;
            } else {
                return;
            }
        } else {
            int read_fd = ctx->transport->fd >= 0 ? ctx->transport->fd : fd;

            if ((events & EPOLLIN) == 0) {
                return;
            }
            n = read(read_fd, ctx->rx_buf + ctx->rx_len, sizeof(ctx->rx_buf) - ctx->rx_len);
            if (n > 0) {
                ctx->rx_len += (size_t)n;
                if (process_transport_frames(ctx) != BC_OK) {
                    ctx->rx_len = 0;
                    return;
                }
                continue;
            }
            if (n == 0) {
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            return;
        }

        ctx->rx_len += (size_t)n;
        if (process_transport_frames(ctx) != BC_OK) {
            ctx->rx_len = 0;
            return;
        }
    }
}

static int register_transport_fds(
    bc_reactor_t *reactor,
    bc_transport_t *transport,
    bc_transport_event_ctx_t *ctx)
{
    int fds[4];
    size_t count = 0;
    size_t i;

    if (transport->ops != NULL && transport->ops->bind_reactor != NULL) {
        return transport->ops->bind_reactor(transport, reactor, on_transport_event, ctx);
    }

    if (transport->ops != NULL && transport->ops->get_fds != NULL &&
        transport->ops->get_fds(transport, fds, &count) == BC_OK && count > 0) {
        for (i = 0; i < count; ++i) {
            uint32_t events = EPOLLIN | EPOLLHUP | EPOLLERR;
            if (bc_reactor_add(reactor, fds[i], events, on_transport_event, ctx) != BC_OK) {
                return BC_ERR_IO;
            }
        }
        return BC_OK;
    }

    if (transport->fd >= 0) {
        return bc_reactor_add(
            reactor,
            transport->fd,
            EPOLLIN | EPOLLHUP | EPOLLERR,
            on_transport_event,
            ctx);
    }

    return BC_OK;
}

int main(int argc, char **argv)
{
    const char *config_path = argc > 1 ? argv[1] : "config/boardcomm.conf";
    bc_config_t config;
    bc_reactor_t reactor;
    bc_transport_manager_t transport_manager;
    bc_router_t router;
    bc_message_bus_t bus;
    bc_client_manager_t client_manager;
    bc_transport_event_ctx_t transport_contexts[BC_MAX_TRANSPORTS];

    (void)bc_log_init("log", "boardcommd");

    if (bc_config_load(config_path, &config) != BC_OK) {
        BC_LOGE("boardcommd", "failed to load config: %s", config_path);
        bc_log_close();
        return EXIT_FAILURE;
    }

    if (bc_reactor_init(&reactor) != BC_OK) {
        BC_LOGE("boardcommd", "failed to initialize reactor");
        bc_log_close();
        return EXIT_FAILURE;
    }

    g_reactor = &reactor;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (bc_transport_manager_init(&transport_manager, &config) != BC_OK) {
        BC_LOGE("boardcommd", "failed to initialize transports");
        bc_reactor_close(&reactor);
        bc_log_close();
        return EXIT_FAILURE;
    }

    bc_router_init(&router, &config, transport_manager.transports, transport_manager.count);
    bc_message_bus_init(&bus, &router);
    bc_message_bus_configure(
        &bus,
        config.node_id,
        transport_manager.transports,
        transport_manager.count,
        config.require_route,
        config.bridge_broadcast);

    if (bc_client_manager_init(
            &client_manager,
            config.socket_path,
            config.socket_uid,
            &reactor,
            &bus) != BC_OK) {
        BC_LOGE("boardcommd", "failed to initialize client IPC: %s", config.socket_path);
        bc_transport_manager_close(&transport_manager);
        bc_reactor_close(&reactor);
        bc_log_close();
        return EXIT_FAILURE;
    }

    memset(transport_contexts, 0, sizeof(transport_contexts));
    for (size_t i = 0; i < transport_manager.count; ++i) {
        transport_contexts[i].transport = &transport_manager.transports[i];
        transport_contexts[i].bus = &bus;
        if (register_transport_fds(&reactor, &transport_manager.transports[i], &transport_contexts[i]) != BC_OK) {
            BC_LOGE("boardcommd", "failed to register transport fds");
            bc_client_manager_close(&client_manager);
            bc_transport_manager_close(&transport_manager);
            bc_reactor_close(&reactor);
            bc_log_close();
            return EXIT_FAILURE;
        }
    }

    BC_LOGI(
        "boardcommd",
        "started: version=%s node=%u socket=%s transports=%zu",
        BOARDCOMM_VERSION,
        config.node_id,
        config.socket_path,
        transport_manager.count);
    (void)bc_reactor_run(&reactor);

    bc_client_manager_close(&client_manager);
    bc_transport_manager_close(&transport_manager);
    bc_reactor_close(&reactor);
    bc_log_close();
    return EXIT_SUCCESS;
}
