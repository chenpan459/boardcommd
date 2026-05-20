#include "message_bus.h"

#include "ipc_protocol.h"
#include "protocol.h"

#include <stdio.h>
#include <string.h>

void bc_message_bus_init(bc_message_bus_t *bus, bc_router_t *router)
{
    memset(bus, 0, sizeof(*bus));
    bus->router = router;
}

void bc_message_bus_set_deliver_fn(
    bc_message_bus_t *bus,
    bc_message_deliver_fn deliver_fn,
    void *deliver_user)
{
    bus->deliver_fn = deliver_fn;
    bus->deliver_user = deliver_user;
}

int bc_message_bus_subscribe(bc_message_bus_t *bus, int client_fd, const char *topic)
{
    if (bus == NULL || topic == NULL || topic[0] == '\0') {
        return BC_ERR_INVALID;
    }
    if (bus->subscription_count >= BC_MAX_SUBSCRIPTIONS) {
        return BC_ERR_NOMEM;
    }

    for (size_t i = 0; i < bus->subscription_count; ++i) {
        if (bus->subscriptions[i].fd == client_fd &&
            strcmp(bus->subscriptions[i].topic, topic) == 0) {
            return BC_OK;
        }
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
        uint8_t frame[sizeof(bc_ipc_header_t) + BC_MAX_CHANNEL_LEN + BC_MAX_TOPIC_LEN + BC_MAX_PAYLOAD_LEN];
        uint16_t channel_len;
        uint16_t topic_len;
        size_t frame_len;

        if (sub->fd == source_fd) {
            continue;
        }
        if (strcmp(sub->topic, msg->topic) != 0 && strcmp(sub->topic, "*") != 0) {
            continue;
        }

        channel_len = (uint16_t)strnlen(msg->channel, BC_MAX_CHANNEL_LEN);
        topic_len = (uint16_t)strnlen(msg->topic, BC_MAX_TOPIC_LEN);
        memset(&header, 0, sizeof(header));
        header.magic = BC_IPC_MAGIC;
        header.version = 1;
        header.type = BC_IPC_DELIVER;
        header.channel_len = channel_len;
        header.topic_len = topic_len;
        header.payload_len = (uint32_t)msg->payload_len;

        frame_len = sizeof(header) + channel_len + topic_len + msg->payload_len;
        memcpy(frame, &header, sizeof(header));
        memcpy(frame + sizeof(header), msg->channel, channel_len);
        memcpy(frame + sizeof(header) + channel_len, msg->topic, topic_len);
        memcpy(frame + sizeof(header) + channel_len + topic_len, msg->payload, msg->payload_len);

        if (bus->deliver_fn == NULL ||
            bus->deliver_fn(bus->deliver_user, sub->fd, frame, frame_len) != BC_OK) {
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
