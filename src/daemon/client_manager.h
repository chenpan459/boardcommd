#ifndef BC_CLIENT_MANAGER_H
#define BC_CLIENT_MANAGER_H

#include "message_bus.h"
#include "reactor.h"

typedef struct bc_client_ctx bc_client_ctx_t;

typedef struct {
    int server_fd;
    char socket_path[128];
    bc_reactor_t *reactor;
    bc_message_bus_t *bus;
    bc_client_ctx_t *clients;
} bc_client_manager_t;

int bc_client_manager_init(
    bc_client_manager_t *manager,
    const char *socket_path,
    bc_reactor_t *reactor,
    bc_message_bus_t *bus);

void bc_client_manager_close(bc_client_manager_t *manager);

#endif
