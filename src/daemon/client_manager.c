#include "client_manager.h"

#include "bc_shm.h"
#include "ipc_protocol.h"
#include "stats.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/types.h>
#endif

#define BC_CLIENT_RX_CAP (sizeof(bc_ipc_header_t) + BC_MAX_CHANNEL_LEN + BC_MAX_TOPIC_LEN + BC_MAX_PAYLOAD_LEN)
#define BC_CLIENT_TX_CAP (1024u * 1024u)

struct bc_client_ctx {
    int fd;
    size_t rx_len;
    uint8_t rx_buf[BC_CLIENT_RX_CAP];
    size_t tx_off;
    size_t tx_len;
    uint8_t tx_buf[BC_CLIENT_TX_CAP];
    bc_shm_session_t shm;
    int shm_ready;
    bc_client_manager_t *manager;
    struct bc_client_ctx *next;
};

static void on_client_event(int fd, uint32_t events, void *user);
static int process_messages(bc_client_ctx_t *client);
static int enqueue_client_tx(bc_client_ctx_t *client, const void *data, size_t len);
static int send_stats_reply(bc_client_ctx_t *client);

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
    if (client->shm_ready) {
        bc_shm_close(&client->shm);
    }
    close(fd);
    free(client);
}

static int send_fd(int fd, int pass_fd)
{
    struct msghdr msg;
    struct iovec iov;
    char byte = 1;
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cmsg;

    memset(&msg, 0, sizeof(msg));
    iov.iov_base = &byte;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &pass_fd, sizeof(int));

    return sendmsg(fd, &msg, MSG_NOSIGNAL) > 0 ? BC_OK : BC_ERR_IO;
}

static int setup_client_shm(bc_client_ctx_t *client)
{
    bc_shm_session_t daemon_side;
    int rc;

    rc = bc_shm_create(&daemon_side);
    if (rc != BC_OK) {
        return rc;
    }

    rc = send_fd(client->fd, daemon_side.fd);
    if (rc != BC_OK) {
        bc_shm_close(&daemon_side);
        return rc;
    }

    client->shm = daemon_side;
    client->shm_ready = 1;
    return BC_OK;
}

static int process_ipc_payload(
    bc_client_ctx_t *client,
    const bc_ipc_header_t *header,
    const char *channel,
    const char *topic,
    const uint8_t *payload)
{
    bc_client_manager_t *manager = client->manager;
    bc_message_t msg;

    if (header->type == BC_IPC_SUBSCRIBE) {
        (void)bc_message_bus_subscribe(manager->bus, client->fd, topic);
        return BC_OK;
    }
    if (header->type == BC_IPC_PUBLISH) {
        memset(&msg, 0, sizeof(msg));
        snprintf(msg.channel, sizeof(msg.channel), "%s", channel);
        snprintf(msg.topic, sizeof(msg.topic), "%s", topic);
        msg.payload_len = header->payload_len;
        memcpy(msg.payload, payload, header->payload_len);
        if (header->version >= BC_IPC_VERSION) {
            msg.dst_node = header->dst_node;
            msg.qos = header->qos;
            msg.flags = header->flags;
            msg.seq = header->seq;
        }
        return bc_message_bus_publish(manager->bus, client->fd, &msg);
    }
    if (header->type == BC_IPC_STATS) {
        return send_stats_reply(client);
    }
    if (header->type == BC_IPC_SHM_SETUP) {
        return setup_client_shm(client);
    }
    if (header->type == BC_IPC_SHM_KICK) {
        return BC_OK;
    }
    return BC_ERR_INVALID;
}

static int process_frame_buffer(bc_client_ctx_t *client, const uint8_t *data, size_t len)
{
    bc_ipc_header_t header;
    char channel[BC_MAX_CHANNEL_LEN] = {0};
    char topic[BC_MAX_TOPIC_LEN] = {0};
    size_t header_size;
    const uint8_t *frame;
    int rc;

    if (len < offsetof(bc_ipc_header_t, dst_node)) {
        return BC_ERR_NOT_FOUND;
    }

    memcpy(&header, data, sizeof(header));
    if (header.magic != BC_IPC_MAGIC) {
        return BC_ERR_INVALID;
    }

    header_size = bc_ipc_header_size(header.version);
    if (len < header_size) {
        return BC_ERR_NOT_FOUND;
    }
    if (header.channel_len >= BC_MAX_CHANNEL_LEN ||
        header.topic_len == 0 ||
        header.topic_len >= BC_MAX_TOPIC_LEN ||
        header.payload_len > BC_MAX_PAYLOAD_LEN) {
        return BC_ERR_INVALID;
    }
    if (len < header_size + header.channel_len + header.topic_len + header.payload_len) {
        return BC_ERR_NOT_FOUND;
    }

    frame = data + header_size;
    memcpy(channel, frame, header.channel_len);
    channel[header.channel_len] = '\0';
    memcpy(topic, frame + header.channel_len, header.topic_len);
    topic[header.topic_len] = '\0';

    rc = process_ipc_payload(
        client,
        &header,
        channel,
        topic,
        frame + header.channel_len + header.topic_len);
    return rc;
}

static int drain_client_shm(bc_client_ctx_t *client)
{
    uint8_t frame[BC_CLIENT_RX_CAP];
    size_t frame_len;
    int rc;

    if (!client->shm_ready) {
        return BC_OK;
    }

    for (;;) {
        rc = bc_shm_ring_read(client->shm.rx, frame, sizeof(frame), &frame_len);
        if (rc == BC_ERR_NOT_FOUND) {
            return BC_OK;
        }
        if (rc != BC_OK) {
            return rc;
        }
        rc = process_frame_buffer(client, frame, frame_len);
        if (rc == BC_ERR_NOMEM) {
            return rc;
        }
        if (rc != BC_OK) {
            return rc;
        }
    }
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

static void resume_pending_publishers(bc_client_manager_t *manager)
{
    for (bc_client_ctx_t *client = manager->clients; client != NULL; client = client->next) {
        int rc;

        if (client->rx_len == 0) {
            continue;
        }
        rc = process_messages(client);
        if (rc == BC_ERR_NOMEM) {
            continue;
        }
        if (rc != BC_OK) {
            close_client(client);
        }
    }
}

static int deliver_to_client(void *user, int client_fd, const void *data, size_t len)
{
    bc_client_manager_t *manager = user;
    bc_client_ctx_t *client = find_client(manager, client_fd);

    if (client == NULL) {
        return BC_ERR_NOT_FOUND;
    }
    if (client->shm_ready) {
        int rc = bc_shm_ring_write(client->shm.tx, data, len);
        if (rc == BC_OK) {
            bc_ipc_header_t kick;
            memset(&kick, 0, sizeof(kick));
            kick.magic = BC_IPC_MAGIC;
            kick.version = BC_IPC_VERSION;
            kick.type = BC_IPC_SHM_KICK;
            (void)enqueue_client_tx(client, &kick, sizeof(kick));
        }
        return rc;
    }
    return enqueue_client_tx(client, data, len);
}

static int send_stats_reply(bc_client_ctx_t *client)
{
    const bc_stats_t *stats = bc_message_bus_stats(client->manager->bus);
    bc_ipc_header_t header;

    if (stats == NULL) {
        return BC_ERR_INVALID;
    }

    memset(&header, 0, sizeof(header));
    header.magic = BC_IPC_MAGIC;
    header.version = BC_IPC_VERSION;
    header.type = BC_IPC_STATS;
    header.payload_len = (uint32_t)sizeof(*stats);
    return enqueue_client_tx(client, &header, sizeof(header)) != BC_OK ||
        enqueue_client_tx(client, stats, sizeof(*stats)) != BC_OK
        ? BC_ERR_IO
        : BC_OK;
}

static int process_one_message(bc_client_ctx_t *client)
{
    size_t frame_len;
    int rc;

    if (client->rx_len < offsetof(bc_ipc_header_t, dst_node)) {
        return BC_ERR_NOT_FOUND;
    }

    {
        bc_ipc_header_t header;
        memcpy(&header, client->rx_buf, sizeof(header));
        if (header.magic != BC_IPC_MAGIC) {
            return BC_ERR_INVALID;
        }
        frame_len = bc_ipc_header_size(header.version) + header.channel_len + header.topic_len +
            header.payload_len;
    }

    if (frame_len > sizeof(client->rx_buf) || client->rx_len < frame_len) {
        return client->rx_len < frame_len ? BC_ERR_NOT_FOUND : BC_ERR_INVALID;
    }

    rc = process_frame_buffer(client, client->rx_buf, frame_len);
    if (rc != BC_OK) {
        return rc;
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
        if (rc == BC_ERR_NOMEM) {
            return BC_ERR_NOMEM;
        }
        if (rc != BC_OK) {
            return rc;
        }
    }
}

static void on_client_event(int fd, uint32_t events, void *user)
{
    bc_client_ctx_t *client = user;
    int rc;

    if ((events & EPOLLOUT) != 0) {
        if (flush_client_tx(client) != BC_OK) {
            close_client(client);
            return;
        }
        resume_pending_publishers(client->manager);
    }

    rc = drain_client_shm(client);
    if (rc == BC_ERR_NOMEM) {
        return;
    }
    if (rc != BC_OK) {
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
            rc = process_messages(client);
            if (rc == BC_ERR_NOMEM) {
                return;
            }
            if (rc != BC_OK) {
                close_client(client);
                return;
            }
            rc = drain_client_shm(client);
            if (rc == BC_ERR_NOMEM) {
                return;
            }
            if (rc != BC_OK) {
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

static int accept_client_auth(bc_client_manager_t *manager, int client_fd)
{
#ifdef SO_PEERCRED
    if (manager->socket_uid > 0) {
        struct ucred cred;
        socklen_t cred_len = sizeof(cred);

        if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0) {
            return BC_ERR_AUTH;
        }
        if ((unsigned int)cred.uid != manager->socket_uid) {
            return BC_ERR_AUTH;
        }
    }
#else
    (void)manager;
    (void)client_fd;
#endif
    return BC_OK;
}

static void on_server_event(int fd, uint32_t events, void *user)
{
    bc_client_manager_t *manager = user;

    (void)fd;
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

        if (accept_client_auth(manager, client_fd) != BC_OK) {
            close(client_fd);
            continue;
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
    unsigned int socket_uid,
    bc_reactor_t *reactor,
    bc_message_bus_t *bus)
{
    struct sockaddr_un addr;

    memset(manager, 0, sizeof(*manager));
    manager->server_fd = -1;
    manager->socket_uid = socket_uid;
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
