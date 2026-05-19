#include "client_manager.h"
#include "boardcomm_version.h"
#include "config.h"
#include "message_bus.h"
#include "protocol.h"
#include "reactor.h"
#include "router.h"
#include "transport_manager.h"
#include "log.h"

#include <errno.h>
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

        (void)bc_message_bus_deliver_local(ctx->bus, -1, &msg);
        memmove(ctx->rx_buf, ctx->rx_buf + frame_len, ctx->rx_len - frame_len);
        ctx->rx_len -= frame_len;
    }
}

static void on_transport_event(int fd, uint32_t events, void *user)
{
    bc_transport_event_ctx_t *ctx = user;

    (void)fd;
    if ((events & EPOLLIN) == 0) {
        return;
    }

    for (;;) {
        ssize_t n;

        if (ctx->rx_len >= sizeof(ctx->rx_buf)) {
            ctx->rx_len = 0;
            return;
        }

        n = read(ctx->transport->fd, ctx->rx_buf + ctx->rx_len, sizeof(ctx->rx_buf) - ctx->rx_len);
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

    if (bc_transport_manager_init(&transport_manager, &config) != BC_OK) {
        BC_LOGE("boardcommd", "failed to initialize transports");
        bc_reactor_close(&reactor);
        bc_log_close();
        return EXIT_FAILURE;
    }

    bc_router_init(&router, &config, transport_manager.transports, transport_manager.count);
    bc_message_bus_init(&bus, &router);

    if (bc_client_manager_init(&client_manager, config.socket_path, &reactor, &bus) != BC_OK) {
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
        (void)bc_reactor_add(
            &reactor,
            transport_manager.transports[i].fd,
            EPOLLIN | EPOLLHUP | EPOLLERR,
            on_transport_event,
            &transport_contexts[i]);
    }

    BC_LOGI(
        "boardcommd",
        "started: version=%s socket=%s transports=%zu",
        BOARDCOMM_VERSION,
        config.socket_path,
        transport_manager.count);
    (void)bc_reactor_run(&reactor);

    bc_client_manager_close(&client_manager);
    bc_transport_manager_close(&transport_manager);
    bc_reactor_close(&reactor);
    bc_log_close();
    return EXIT_SUCCESS;
}
