#include "client_manager.h"
#include "boardcomm_version.h"
#include "config.h"
#include "message_bus.h"
#include "protocol.h"
#include "reactor.h"
#include "router.h"
#include "transport_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

typedef struct {
    bc_transport_t *transport;
    bc_message_bus_t *bus;
} bc_transport_event_ctx_t;

static void on_transport_event(int fd, uint32_t events, void *user)
{
    bc_transport_event_ctx_t *ctx = user;
    uint8_t buf[BC_MAX_FRAME_LEN];
    ssize_t n;
    bc_message_t msg;

    (void)fd;
    if ((events & EPOLLIN) == 0) {
        return;
    }

    n = read(ctx->transport->fd, buf, sizeof(buf));
    if (n <= 0) {
        return;
    }

    if (bc_protocol_decode(buf, (size_t)n, &msg) == BC_OK) {
        (void)bc_message_bus_deliver_local(ctx->bus, -1, &msg);
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

    if (bc_config_load(config_path, &config) != BC_OK) {
        fprintf(stderr, "failed to load config: %s\n", config_path);
        return EXIT_FAILURE;
    }

    if (bc_reactor_init(&reactor) != BC_OK) {
        fprintf(stderr, "failed to initialize reactor\n");
        return EXIT_FAILURE;
    }

    if (bc_transport_manager_init(&transport_manager, &config) != BC_OK) {
        fprintf(stderr, "failed to initialize transports\n");
        bc_reactor_close(&reactor);
        return EXIT_FAILURE;
    }

    bc_router_init(&router, &config, transport_manager.transports, transport_manager.count);
    bc_message_bus_init(&bus, &router);

    if (bc_client_manager_init(&client_manager, config.socket_path, &reactor, &bus) != BC_OK) {
        fprintf(stderr, "failed to initialize client IPC: %s\n", config.socket_path);
        bc_transport_manager_close(&transport_manager);
        bc_reactor_close(&reactor);
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

    printf(
        "boardcommd started: version=%s socket=%s transports=%zu\n",
        BOARDCOMM_VERSION,
        config.socket_path,
        transport_manager.count);
    (void)bc_reactor_run(&reactor);

    bc_client_manager_close(&client_manager);
    bc_transport_manager_close(&transport_manager);
    bc_reactor_close(&reactor);
    return EXIT_SUCCESS;
}
