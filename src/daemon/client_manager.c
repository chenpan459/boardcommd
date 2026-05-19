#include "client_manager.h"

#include "ipc_protocol.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int read_all(int fd, void *data, size_t len)
{
    uint8_t *p = data;
    size_t off = 0;

    while (off < len) {
        ssize_t n = read(fd, p + off, len - off);

        if (n == 0) {
            return BC_ERR_IO;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return BC_ERR_IO;
        }
        off += (size_t)n;
    }
    return BC_OK;
}

static void close_client(bc_client_manager_t *manager, int fd)
{
    bc_reactor_del(manager->reactor, fd);
    bc_message_bus_remove_client(manager->bus, fd);
    close(fd);
}

static void on_client_event(int fd, uint32_t events, void *user)
{
    bc_client_manager_t *manager = user;
    bc_ipc_header_t header;
    char topic[BC_MAX_TOPIC_LEN] = {0};
    bc_message_t msg;

    if ((events & EPOLLIN) == 0 && (events & (EPOLLHUP | EPOLLERR)) != 0) {
        close_client(manager, fd);
        return;
    }

    if (read_all(fd, &header, sizeof(header)) != BC_OK ||
        header.magic != BC_IPC_MAGIC ||
        header.topic_len == 0 ||
        header.topic_len >= BC_MAX_TOPIC_LEN ||
        header.payload_len > BC_MAX_PAYLOAD_LEN) {
        close_client(manager, fd);
        return;
    }

    if (read_all(fd, topic, header.topic_len) != BC_OK) {
        close_client(manager, fd);
        return;
    }
    topic[header.topic_len] = '\0';

    if (header.type == BC_IPC_SUBSCRIBE) {
        (void)bc_message_bus_subscribe(manager->bus, fd, topic);
        return;
    }

    if (header.type != BC_IPC_PUBLISH) {
        close_client(manager, fd);
        return;
    }

    memset(&msg, 0, sizeof(msg));
    snprintf(msg.topic, sizeof(msg.topic), "%s", topic);
    msg.payload_len = header.payload_len;
    if (read_all(fd, msg.payload, msg.payload_len) != BC_OK) {
        close_client(manager, fd);
        return;
    }

    (void)bc_message_bus_publish(manager->bus, fd, &msg);
}

static void on_server_event(int fd, uint32_t events, void *user)
{
    bc_client_manager_t *manager = user;

    (void)events;
    for (;;) {
        int client_fd = accept4(fd, NULL, NULL, SOCK_CLOEXEC);

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        (void)bc_reactor_add(manager->reactor, client_fd, EPOLLIN | EPOLLHUP | EPOLLERR, on_client_event, manager);
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

    return bc_reactor_add(reactor, manager->server_fd, EPOLLIN, on_server_event, manager);
}

void bc_client_manager_close(bc_client_manager_t *manager)
{
    if (manager->server_fd >= 0) {
        close(manager->server_fd);
        manager->server_fd = -1;
    }
    unlink(manager->socket_path);
}
