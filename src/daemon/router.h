#ifndef BC_ROUTER_H
#define BC_ROUTER_H

#include "bc_types.h"
#include "config.h"
#include "transport.h"

typedef struct {
    bc_transport_t *transport;
} bc_route_result_t;

typedef struct {
    const bc_config_t *config;
    bc_transport_t *transports;
    size_t transport_count;
} bc_router_t;

void bc_router_init(
    bc_router_t *router,
    const bc_config_t *config,
    bc_transport_t *transports,
    size_t transport_count);

int bc_router_route(
    bc_router_t *router,
    const bc_message_t *msg,
    bc_route_result_t *result);

#endif
