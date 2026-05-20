#include "client_manager.h"

#include "ipc_protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define BC_CLIENT_RX_CAP (sizeof(bc_ipc_header_t) + BC_MAX_CHANNEL_LEN + BC_MAX_TOPIC_LEN + BC_MAX_PAYLOAD_LEN)
#define BC_CLIENT_TX_CAP (256u * 1024u)

struct bc_client_ctx {
    int fd;
    size_t rx_len;
    uint8_t rx_buf[BC_CLIENT_RX_CAP];
    size_t tx_off;
    size_t tx_len;
    uint8_t tx_buf[BC_CLIENT_TX_CAP];
    bc_client_manager_t *manager;
    struct bc_client_ctx *next;
};

static void on_client_event(int fd, uint32_t events, void *user);

static void link_client(bc_client_manager_t *manager, bc_client_ctx_t *client)
{
    client->next = manager->clients;
    manager->clients = client;
}

static void unlink_client(bc_client_manager_t *manager, bc_client_ctx_t *client)
{
    bc_client_ctx_t **current = &manager->clients;

    while (*current != NULL) {
        if (*current == client) {
            *current = client->next;
            return;
        }
        current = &(*current)->next;
    }
}

static bc_client_ctx_t *find_client(bc_client_manager_t *manager, int fd)
{
    for (bc_client_ctx_t *client = manager->clients; client != NULL; client = client->next) {
        if (client->fd == fd) {
            return client;
        }
    }
    return NULL;
}

static void close_client(bc_client_ctx_t *client)
{
    bc_client_manager_t *manager = client->manager;
    int fd = client->fd;

    bc_reactor_del(manager->reactor, fd);
    bc_message_bus_remove_client(manager->bus, fd);
    unlink_client(manager, client);
    close(fd);
    free(client);
}

static int update_client_events(bc_client_ctx_t *client)
{
    uint32_t events = EPOLLIN | EPOLLHUP | EPOLLERR;

    if (client->tx_len > 0) {
        events |= EPOLLOUT;
    }

    return bc_reactor_mod(client->manager->reactor, client->fd, events, on_client_event, client);
}

static int flush_client_tx(bc_client_ctx_t *client)
{
    while (client->tx_len > 0) {
        ssize_t n = send(
            client->fd,
            client->tx_buf + client->tx_off,
            client->tx_len,
            MSG_NOSIGNAL);

        if (n > 0) {
            client->tx_off += (size_t)n;
            client->tx_len -= (size_t)n;
            if (client->tx_len == 0) {
                client->tx_off = 0;
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

    return update_client_events(client);
}

static int enqueue_client_tx(bc_client_ctx_t *client, const void *data, size_t len)
{
    if (len > sizeof(client->tx_buf)) {
        return BC_ERR_INVALID;
    }

    if (client->tx_off > 0 && client->tx_off + client->tx_len + len > sizeof(client->tx_buf)) {
        memmove(client->tx_buf, client->tx_buf + client->tx_off, client->tx_len);
        client->tx_off = 0;
    }

    if (client->tx_off + client->tx_len + len > sizeof(client->tx_buf)) {
        return BC_ERR_NOMEM;
    }

    memcpy(client->tx_buf + client->tx_off + client->tx_len, data, len);
    client->tx_len += len;
    return update_client_events(client);
}

static int deliver_to_client(void *user, int client_fd, const void *data, size_t len)
{
    bc_client_manager_t *manager = user;
    bc_client_ctx_t *client = find_client(manager, client_fd);

    if (client == NULL) {
        return BC_ERR_NOT_FOUND;
    }
    return enqueue_client_tx(client, data, len);
}

static int process_one_message(bc_client_ctx_t *client)
{
    bc_client_manager_t *manager = client->manager;
    bc_ipc_header_t header;
    char channel[BC_MAX_CHANNEL_LEN] = {0};
    char topic[BC_MAX_TOPIC_LEN] = {0};
    bc_message_t msg;
    size_t frame_len;
    const uint8_t *frame;

    if (client->rx_len < sizeof(header)) {
        return BC_ERR_NOT_FOUND;
    }

    memcpy(&header, client->rx_buf, sizeof(header));
    if (
        header.magic != BC_IPC_MAGIC ||
        header.channel_len >= BC_MAX_CHANNEL_LEN ||
        header.topic_len == 0 ||
        header.topic_len >= BC_MAX_TOPIC_LEN ||
        header.payload_len > BC_MAX_PAYLOAD_LEN) {
        return BC_ERR_INVALID;
    }

    frame_len = sizeof(header) + header.channel_len + header.topic_len + header.payload_len;
    if (frame_len > sizeof(client->rx_buf)) {
        return BC_ERR_INVALID;
    }
    if (client->rx_len < frame_len) {
        return BC_ERR_NOT_FOUND;
    }

    frame = client->rx_buf + sizeof(header);
    memcpy(channel, frame, header.channel_len);
    channel[header.channel_len] = '\0';
    memcpy(topic, frame + header.channel_len, header.topic_len);
    topic[header.topic_len] = '\0';

    if (header.type == BC_IPC_SUBSCRIBE) {
        (void)bc_message_bus_subscribe(manager->bus, client->fd, topic);
    } else if (header.type == BC_IPC_PUBLISH) {
        memset(&msg, 0, sizeof(msg));
        snprintf(msg.channel, sizeof(msg.channel), "%s", channel);
        snprintf(msg.topic, sizeof(msg.topic), "%s", topic);
        msg.payload_len = header.payload_len;
        memcpy(msg.payload, frame + header.channel_len + header.topic_len, msg.payload_len);
        (void)bc_message_bus_publish(manager->bus, client->fd, &msg);
    } else {
        return BC_ERR_INVALID;
    }

    memmove(client->rx_buf, client->rx_buf + frame_len, client->rx_len - frame_len);
    client->rx_len -= frame_len;
    return BC_OK;
}

static int process_messages(bc_client_ctx_t *client)
{
    for (;;) {
        int rc = process_one_message(client);

        if (rc == BC_ERR_NOT_FOUND) {
            return BC_OK;
        }
        if (rc != BC_OK) {
            return rc;
        }
    }
}

static void on_client_event(int fd, uint32_t events, void *user)
{
    bc_client_ctx_t *client = user;

    if ((events & EPOLLOUT) != 0 && flush_client_tx(client) != BC_OK) {
        close_client(client);
        return;
    }

    if ((events & (EPOLLHUP | EPOLLERR)) != 0 && (events & EPOLLIN) == 0) {
        close_client(client);
        return;
    }

    for (;;) {
        ssize_t n;

        if (client->rx_len >= sizeof(client->rx_buf)) {
            close_client(client);
            return;
        }

        n = read(fd, client->rx_buf + client->rx_len, sizeof(client->rx_buf) - client->rx_len);
        if (n > 0) {
            client->rx_len += (size_t)n;
            if (process_messages(client) != BC_OK) {
                close_client(client);
                return;
            }
            continue;
        }
        if (n == 0) {
            close_client(client);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        close_client(client);
        return;
    }
}

static void on_server_event(int fd, uint32_t events, void *user)
{
    bc_client_manager_t *manager = user;

    (void)events;
    for (;;) {
        int client_fd = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        bc_client_ctx_t *client;

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        client = calloc(1, sizeof(*client));
        if (client == NULL) {
            close(client_fd);
            continue;
        }
        client->fd = client_fd;
        client->manager = manager;
        link_client(manager, client);

        if (bc_reactor_add(manager->reactor, client_fd, EPOLLIN | EPOLLHUP | EPOLLERR, on_client_event, client) != BC_OK) {
            unlink_client(manager, client);
            close(client_fd);
            free(client);
        }
    }
}

int bc_client_manager_init(
    bc_client_manager_t *manager,
    const char *socket_path,
    bc_reactor_t *reactor,
    bc_message_bus_t *bus)
{
    struct sockaddr_un addr;

    memset(manager, 0, sizeof(*manager));
    manager->server_fd = -1;
    manager->reactor = reactor;
    manager->bus = bus;
    snprintf(manager->socket_path, sizeof(manager->socket_path), "%s", socket_path);

    manager->server_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (manager->server_fd < 0) {
        return BC_ERR_IO;
    }

    unlink(manager->socket_path);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(manager->socket_path) >= sizeof(addr.sun_path)) {
        close(manager->server_fd);
        manager->server_fd = -1;
        return BC_ERR_INVALID;
    }
    memcpy(addr.sun_path, manager->socket_path, strlen(manager->socket_path) + 1);

    if (bind(manager->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(manager->server_fd, 16) < 0) {
        close(manager->server_fd);
        manager->server_fd = -1;
        return BC_ERR_IO;
    }

    bc_message_bus_set_deliver_fn(bus, deliver_to_client, manager);
    return bc_reactor_add(reactor, manager->server_fd, EPOLLIN, on_server_event, manager);
}

void bc_client_manager_close(bc_client_manager_t *manager)
{
    while (manager->clients != NULL) {
        close_client(manager->clients);
    }

    if (manager->server_fd >= 0) {
        bc_reactor_del(manager->reactor, manager->server_fd);
        close(manager->server_fd);
        manager->server_fd = -1;
    }
    unlink(manager->socket_path);
}
