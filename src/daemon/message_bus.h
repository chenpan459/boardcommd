#ifndef BC_MESSAGE_BUS_H
#define BC_MESSAGE_BUS_H

#include "bc_types.h"
#include "fragment.h"
#include "router.h"
#include "stats.h"

#define BC_MAX_CLIENTS 64
#define BC_MAX_SUBSCRIPTIONS 128
#define BC_MAX_QOS_PENDING 64

typedef struct {
    int fd;
    char topic[BC_MAX_TOPIC_LEN];
} bc_subscription_t;

typedef int (*bc_message_deliver_fn)(void *user, int client_fd, const void *data, size_t len);

typedef struct bc_transport bc_transport_t;

typedef struct {
    bc_subscription_t subscriptions[BC_MAX_SUBSCRIPTIONS];
    size_t subscription_count;
    bc_router_t *router;
    bc_message_deliver_fn deliver_fn;
    void *deliver_user;
    bc_transport_t *transports;
    size_t transport_count;
    uint32_t node_id;
    uint32_t next_seq;
    int require_route;
    int bridge_broadcast;
    bc_stats_t stats;
    bc_reasm_t *reasm;
} bc_message_bus_t;

void bc_message_bus_init(bc_message_bus_t *bus, bc_router_t *router);
void bc_message_bus_configure(
    bc_message_bus_t *bus,
    uint32_t node_id,
    bc_transport_t *transports,
    size_t transport_count,
    int require_route,
    int bridge_broadcast);
void bc_message_bus_set_deliver_fn(
    bc_message_bus_t *bus,
    bc_message_deliver_fn deliver_fn,
    void *deliver_user);
const bc_stats_t *bc_message_bus_stats(const bc_message_bus_t *bus);
int bc_message_bus_subscribe(bc_message_bus_t *bus, int client_fd, const char *topic);
void bc_message_bus_remove_client(bc_message_bus_t *bus, int client_fd);
int bc_message_bus_publish(bc_message_bus_t *bus, int source_fd, bc_message_t *msg);
int bc_message_bus_deliver_local(bc_message_bus_t *bus, int source_fd, const bc_message_t *msg);
int bc_message_bus_handle_inbound(
    bc_message_bus_t *bus,
    bc_message_t *msg,
    bc_transport_t *from_transport);

#endif
