#include "message_bus.h"

#include "ipc_protocol.h"
#include "protocol.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int write_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);

        if (n <= 0) {
            return BC_ERR_IO;
        }
        off += (size_t)n;
    }
    return BC_OK;
}

void bc_message_bus_init(bc_message_bus_t *bus, bc_router_t *router)
{
    memset(bus, 0, sizeof(*bus));
    bus->router = router;
}

int bc_message_bus_subscribe(bc_message_bus_t *bus, int client_fd, const char *topic)
{
    if (bus == NULL || topic == NULL || topic[0] == '\0') {
        return BC_ERR_INVALID;
    }
    if (bus->subscription_count >= BC_MAX_SUBSCRIPTIONS) {
        return BC_ERR_NOMEM;
    }

    bus->subscriptions[bus->subscription_count].fd = client_fd;
    snprintf(
        bus->subscriptions[bus->subscription_count].topic,
        sizeof(bus->subscriptions[bus->subscription_count].topic),
        "%s",
        topic);
    bus->subscription_count++;
    return BC_OK;
}

void bc_message_bus_remove_client(bc_message_bus_t *bus, int client_fd)
{
    size_t out = 0;

    for (size_t i = 0; i < bus->subscription_count; ++i) {
        if (bus->subscriptions[i].fd != client_fd) {
            if (out != i) {
                bus->subscriptions[out] = bus->subscriptions[i];
            }
            out++;
        }
    }
    bus->subscription_count = out;
}

int bc_message_bus_deliver_local(bc_message_bus_t *bus, int source_fd, const bc_message_t *msg)
{
    int delivered = 0;

    for (size_t i = 0; i < bus->subscription_count; ++i) {
        bc_subscription_t *sub = &bus->subscriptions[i];
        bc_ipc_header_t header;
        uint16_t topic_len;

        if (sub->fd == source_fd) {
            continue;
        }
        if (strcmp(sub->topic, msg->topic) != 0 && strcmp(sub->topic, "*") != 0) {
            continue;
        }

        topic_len = (uint16_t)strnlen(msg->topic, BC_MAX_TOPIC_LEN);
        memset(&header, 0, sizeof(header));
        header.magic = BC_IPC_MAGIC;
        header.version = 1;
        header.type = BC_IPC_DELIVER;
        header.topic_len = topic_len;
        header.payload_len = (uint32_t)msg->payload_len;

        if (write_all(sub->fd, &header, sizeof(header)) != BC_OK ||
            write_all(sub->fd, msg->topic, topic_len) != BC_OK ||
            write_all(sub->fd, msg->payload, msg->payload_len) != BC_OK) {
            continue;
        }
        delivered++;
    }

    return delivered;
}

int bc_message_bus_publish(bc_message_bus_t *bus, int source_fd, const bc_message_t *msg)
{
    bc_route_result_t route;
    bc_frame_t frame;

    if (bus == NULL || msg == NULL) {
        return BC_ERR_INVALID;
    }

    (void)bc_message_bus_deliver_local(bus, source_fd, msg);

    if (bc_router_route(bus->router, msg, &route) != BC_OK) {
        return BC_OK;
    }
    if (bc_protocol_encode(msg, &frame) != BC_OK) {
        return BC_ERR_INVALID;
    }
    if (route.transport->ops == NULL || route.transport->ops->send == NULL) {
        return BC_ERR_INVALID;
    }

    return route.transport->ops->send(route.transport, frame.data, frame.len);
}
