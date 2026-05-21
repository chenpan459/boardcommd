#include "message_bus.h"

#include "fragment.h"
#include "ipc_protocol.h"
#include "protocol.h"
#include "topic_match.h"
#include "transport.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    bc_transport_t *transport;
    bc_message_bus_t *bus;
} bc_send_ctx_t;

static bc_reasm_t g_reasm;

static int emit_network_frame(const bc_message_t *piece, void *user)
{
    bc_send_ctx_t *ctx = user;
    bc_frame_t frame;
    int rc;

    if (bc_protocol_encode(piece, &frame) != BC_OK) {
        return BC_ERR_INVALID;
    }
    if (ctx->transport->ops == NULL || ctx->transport->ops->send == NULL) {
        return BC_ERR_INVALID;
    }
    rc = ctx->transport->ops->send(ctx->transport, frame.data, frame.len);
    if (rc != BC_OK) {
        ctx->bus->stats.pub_failed++;
    }
    return rc;
}

static int send_on_transport(bc_message_bus_t *bus, bc_transport_t *transport, bc_message_t *msg)
{
    bc_send_ctx_t ctx = {.transport = transport, .bus = bus};
    int rc;

    if (transport == NULL) {
        return BC_ERR_INVALID;
    }
    if (msg->payload_len > BC_FRAG_CHUNK_MAX) {
        bus->stats.frag_tx++;
        rc = bc_fragment_publish(msg, emit_network_frame, &ctx);
    } else {
        rc = emit_network_frame(msg, &ctx);
    }
    if (rc == BC_OK) {
        bus->stats.pub_network++;
    }
    return rc;
}

static int send_ack(bc_message_bus_t *bus, bc_transport_t *transport, const bc_message_t *req)
{
    bc_message_t ack;

    memset(&ack, 0, sizeof(ack));
    ack.src_node = bus->node_id;
    ack.dst_node = req->src_node;
    ack.seq = req->seq;
    ack.flags = BC_FLAG_ACK;
    ack.qos = BC_QOS_AT_MOST_ONCE;
    snprintf(ack.topic, sizeof(ack.topic), "%s", req->topic);
    snprintf(ack.channel, sizeof(ack.channel), "%s", req->channel);

    if (send_on_transport(bus, transport, &ack) == BC_OK) {
        bus->stats.ack_sent++;
        return BC_OK;
    }
    return BC_ERR_IO;
}

static int should_deliver_local(const bc_message_bus_t *bus, const bc_message_t *msg)
{
    return msg->dst_node == 0 || msg->dst_node == bus->node_id;
}

static int should_forward(const bc_message_bus_t *bus, const bc_message_t *msg)
{
    if (msg->dst_node == 0) {
        return bus->bridge_broadcast;
    }
    return msg->dst_node != bus->node_id;
}

static int forward_message(
    bc_message_bus_t *bus,
    bc_message_t *msg,
    bc_transport_t *exclude)
{
    bc_route_result_t route;
    int rc;

    if (bc_router_route(bus->router, msg, &route) != BC_OK) {
        if (bus->require_route) {
            bus->stats.pub_failed++;
            return BC_ERR_NOT_FOUND;
        }
        return BC_OK;
    }
    if (route.transport == exclude) {
        return BC_OK;
    }

    rc = send_on_transport(bus, route.transport, msg);
    if (rc == BC_OK) {
        bus->stats.rx_forward++;
    }
    return rc;
}

void bc_message_bus_init(bc_message_bus_t *bus, bc_router_t *router)
{
    memset(bus, 0, sizeof(*bus));
    bus->router = router;
    bus->bridge_broadcast = 1;
    bc_reasm_init(&g_reasm);
    bus->reasm = &g_reasm;
    bc_stats_reset(&bus->stats);
}

void bc_message_bus_configure(
    bc_message_bus_t *bus,
    uint32_t node_id,
    bc_transport_t *transports,
    size_t transport_count,
    int require_route,
    int bridge_broadcast)
{
    bus->node_id = node_id;
    bus->transports = transports;
    bus->transport_count = transport_count;
    bus->require_route = require_route;
    bus->bridge_broadcast = bridge_broadcast;
    bus->next_seq = 0;
}

void bc_message_bus_set_deliver_fn(
    bc_message_bus_t *bus,
    bc_message_deliver_fn deliver_fn,
    void *deliver_user)
{
    bus->deliver_fn = deliver_fn;
    bus->deliver_user = deliver_user;
}

const bc_stats_t *bc_message_bus_stats(const bc_message_bus_t *bus)
{
    return bus != NULL ? &bus->stats : NULL;
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
    int matched = 0;
    int failed = 0;

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
        if (!bc_topic_matches(sub->topic, msg->topic)) {
            continue;
        }
        matched++;

        channel_len = (uint16_t)strnlen(msg->channel, BC_MAX_CHANNEL_LEN);
        topic_len = (uint16_t)strnlen(msg->topic, BC_MAX_TOPIC_LEN);
        memset(&header, 0, sizeof(header));
        header.magic = BC_IPC_MAGIC;
        header.version = BC_IPC_VERSION;
        header.type = BC_IPC_DELIVER;
        header.channel_len = channel_len;
        header.topic_len = topic_len;
        header.payload_len = (uint32_t)msg->payload_len;
        header.dst_node = msg->dst_node;
        header.qos = msg->qos;
        header.flags = msg->flags;
        header.seq = msg->seq;

        frame_len = sizeof(header) + channel_len + topic_len + msg->payload_len;
        memcpy(frame, &header, sizeof(header));
        memcpy(frame + sizeof(header), msg->channel, channel_len);
        memcpy(frame + sizeof(header) + channel_len, msg->topic, topic_len);
        memcpy(frame + sizeof(header) + channel_len + topic_len, msg->payload, msg->payload_len);

        if (bus->deliver_fn == NULL ||
            bus->deliver_fn(bus->deliver_user, sub->fd, frame, frame_len) != BC_OK) {
            failed++;
            continue;
        }
        delivered++;
    }

    if (delivered > 0) {
        bus->stats.rx_local += (uint64_t)delivered;
    }
    if (failed > 0) {
        return BC_ERR_NOMEM;
    }
    if (matched > 0 && delivered == 0) {
        return BC_ERR_NOMEM;
    }

    return delivered;
}

int bc_message_bus_publish(bc_message_bus_t *bus, int source_fd, bc_message_t *msg)
{
    bc_route_result_t route;
    int local_rc = BC_OK;
    int network_rc = BC_OK;
    int need_network;

    if (bus == NULL || msg == NULL) {
        return BC_ERR_INVALID;
    }

    msg->src_node = bus->node_id;
    if (msg->seq == 0) {
        msg->seq = ++bus->next_seq;
    }

    need_network = should_forward(bus, msg) || msg->channel[0] != '\0' ||
        (msg->dst_node != 0 && msg->dst_node != bus->node_id);

    if (should_deliver_local(bus, msg)) {
        local_rc = bc_message_bus_deliver_local(bus, source_fd, msg);
        if (local_rc == BC_ERR_NOMEM) {
            /* keep error */
        } else if (local_rc > 0) {
            bus->stats.pub_local += (uint64_t)local_rc;
            local_rc = BC_OK;
        } else {
            local_rc = BC_OK;
        }
    }

    if (need_network && bc_router_route(bus->router, msg, &route) != BC_OK) {
        if (bus->require_route) {
            bus->stats.pub_failed++;
            return local_rc == BC_ERR_NOMEM ? BC_ERR_NOMEM : BC_ERR_NOT_FOUND;
        }
        return local_rc;
    }

    if (need_network && route.transport != NULL) {
        network_rc = send_on_transport(bus, route.transport, msg);
        if (network_rc != BC_OK) {
            return network_rc;
        }
    }

    if (local_rc == BC_ERR_NOMEM) {
        return BC_ERR_NOMEM;
    }
    return BC_OK;
}

int bc_message_bus_handle_inbound(
    bc_message_bus_t *bus,
    bc_message_t *msg,
    bc_transport_t *from_transport)
{
    bc_message_t complete;
    int rc;

    if (bus == NULL || msg == NULL) {
        return BC_ERR_INVALID;
    }

    bus->stats.rx_network++;

    if ((msg->flags & BC_FLAG_ACK) != 0) {
        bus->stats.ack_recv++;
        return BC_OK;
    }

    if ((msg->flags & BC_FLAG_FRAGMENT) != 0) {
        bus->stats.frag_rx++;
        rc = bc_reasm_feed(bus->reasm, msg, &complete);
        if (rc == BC_ERR_NOT_FOUND) {
            return BC_OK;
        }
        if (rc != BC_OK) {
            bus->stats.rx_drop++;
            return rc;
        }
        msg = &complete;
    }

    if (msg->dst_node != 0 && msg->dst_node != bus->node_id) {
        rc = forward_message(bus, msg, from_transport);
        if (rc != BC_OK) {
            bus->stats.rx_drop++;
        }
        return rc;
    }

    if (should_deliver_local(bus, msg)) {
        rc = bc_message_bus_deliver_local(bus, -1, msg);
        if (rc == BC_ERR_NOMEM) {
            bus->stats.rx_drop++;
            return rc;
        }
        if (rc > 0) {
            bus->stats.pub_local += (uint64_t)rc;
        }
    }

    if (should_forward(bus, msg)) {
        rc = forward_message(bus, msg, from_transport);
        if (rc != BC_OK && bus->require_route) {
            return rc;
        }
    }

    if (msg->qos == BC_QOS_AT_LEAST_ONCE && from_transport != NULL) {
        (void)send_ack(bus, from_transport, msg);
    }

    return BC_OK;
}
