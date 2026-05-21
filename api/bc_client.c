#include "bc.h"

#include "ipc_protocol.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int g_fd = -1;
static bc_msg_cb g_cb;
static void *g_user;

static int write_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t off = 0;

    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, MSG_NOSIGNAL);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return BC_ERR_IO;
        }
        off += (size_t)n;
    }
    return BC_OK;
}

static int read_all(int fd, void *data, size_t len)
{
    uint8_t *p = data;
    size_t off = 0;

    while (off < len) {
        ssize_t n = read(fd, p + off, len - off);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return BC_ERR_IO;
        }
        off += (size_t)n;
    }
    return BC_OK;
}

static int ensure_connected(void)
{
    if (g_fd >= 0) {
        return BC_OK;
    }
    return bc_init(NULL);
}

int bc_open(const char *socket_path)
{
    struct sockaddr_un addr;
    const char *path = socket_path != NULL ? socket_path : BC_DEFAULT_SOCKET_PATH;
    int fd;

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return BC_ERR_IO;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        close(fd);
        return BC_ERR_INVALID;
    }
    memcpy(addr.sun_path, path, strlen(path) + 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return BC_ERR_IO;
    }

    return fd;
}

int bc_close(int handle)
{
    if (handle < 0) {
        return BC_ERR_INVALID;
    }

    return close(handle) == 0 ? BC_OK : BC_ERR_IO;
}

static int send_ipc_message(
    int handle,
    uint16_t type,
    const char *channel,
    const char *topic,
    const void *payload,
    size_t len)
{
    bc_ipc_header_t header;
    size_t channel_len = 0;
    size_t topic_len;

    if (handle < 0) {
        return BC_ERR_INVALID;
    }
    if (topic == NULL || topic[0] == '\0') {
        return BC_ERR_INVALID;
    }
    if (len > BC_MAX_PAYLOAD_LEN) {
        return BC_ERR_INVALID;
    }

    topic_len = strnlen(topic, BC_MAX_TOPIC_LEN);
    if (topic_len == 0 || topic_len >= BC_MAX_TOPIC_LEN) {
        return BC_ERR_INVALID;
    }
    if (channel != NULL && channel[0] != '\0') {
        channel_len = strnlen(channel, BC_MAX_CHANNEL_LEN);
        if (channel_len == 0 || channel_len >= BC_MAX_CHANNEL_LEN) {
            return BC_ERR_INVALID;
        }
    }

    memset(&header, 0, sizeof(header));
    header.magic = BC_IPC_MAGIC;
    header.version = 1;
    header.type = type;
    header.topic_len = (uint16_t)topic_len;
    header.channel_len = (uint16_t)channel_len;
    header.payload_len = (uint32_t)len;

    if (write_all(handle, &header, sizeof(header)) != BC_OK ||
        (channel_len > 0 && write_all(handle, channel, channel_len) != BC_OK) ||
        write_all(handle, topic, topic_len) != BC_OK) {
        return BC_ERR_IO;
    }
    if (len > 0 && write_all(handle, payload, len) != BC_OK) {
        return BC_ERR_IO;
    }

    return BC_OK;
}

ssize_t bc_write(int handle, const char *topic, const void *payload, size_t len)
{
    int rc = send_ipc_message(handle, BC_IPC_PUBLISH, NULL, topic, payload, len);

    return rc == BC_OK ? (ssize_t)len : (ssize_t)rc;
}

ssize_t bc_write_channel(
    int handle,
    const char *channel,
    const char *topic,
    const void *payload,
    size_t len)
{
    int rc = send_ipc_message(handle, BC_IPC_PUBLISH, channel, topic, payload, len);

    return rc == BC_OK ? (ssize_t)len : (ssize_t)rc;
}

int bc_subscribe_fd(int handle, const char *topic)
{
    return send_ipc_message(handle, BC_IPC_SUBSCRIBE, NULL, topic, NULL, 0);
}

ssize_t bc_read(
    int handle,
    char *topic,
    size_t topic_cap,
    void *payload,
    size_t payload_cap,
    int timeout_ms)
{
    struct pollfd pfd;
    bc_ipc_header_t header;
    char channel[BC_MAX_CHANNEL_LEN];
    int rc;

    if (handle < 0 || topic == NULL || topic_cap == 0 || payload == NULL) {
        return BC_ERR_INVALID;
    }

    pfd.fd = handle;
    pfd.events = POLLIN;
    pfd.revents = 0;
    do {
        rc = poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);

    if (rc == 0) {
        return BC_ERR_TIMEOUT;
    }
    if (rc < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return BC_ERR_IO;
    }

    if (read_all(handle, &header, sizeof(header)) != BC_OK ||
        header.magic != BC_IPC_MAGIC ||
        header.type != BC_IPC_DELIVER ||
        header.channel_len >= BC_MAX_CHANNEL_LEN ||
        header.topic_len == 0 ||
        header.topic_len >= BC_MAX_TOPIC_LEN ||
        header.payload_len > BC_MAX_PAYLOAD_LEN ||
        topic_cap <= header.topic_len ||
        payload_cap < header.payload_len) {
        return BC_ERR_IO;
    }

    if ((header.channel_len > 0 && read_all(handle, channel, header.channel_len) != BC_OK) ||
        read_all(handle, topic, header.topic_len) != BC_OK ||
        read_all(handle, payload, header.payload_len) != BC_OK) {
        return BC_ERR_IO;
    }
    topic[header.topic_len] = '\0';

    return (ssize_t)header.payload_len;
}

int bc_init(const char *socket_path)
{
    if (g_fd >= 0) {
        return BC_OK;
    }

    g_fd = bc_open(socket_path);
    return g_fd >= 0 ? BC_OK : g_fd;
}

void bc_shutdown(void)
{
    if (g_fd >= 0) {
        (void)bc_close(g_fd);
        g_fd = -1;
    }
    g_cb = NULL;
    g_user = NULL;
}

int bc_publish(const char *topic, const void *payload, size_t len)
{
    if (ensure_connected() != BC_OK) {
        return BC_ERR_IO;
    }

    return bc_write(g_fd, topic, payload, len) >= 0 ? BC_OK : BC_ERR_IO;
}

int bc_subscribe(const char *topic, bc_msg_cb cb, void *user)
{
    int rc;

    if (cb == NULL) {
        return BC_ERR_INVALID;
    }

    if (ensure_connected() != BC_OK) {
        return BC_ERR_IO;
    }

    rc = bc_subscribe_fd(g_fd, topic);
    if (rc == BC_OK) {
        g_cb = cb;
        g_user = user;
    }
    return rc;
}

int bc_poll(int timeout_ms)
{
    char topic[BC_MAX_TOPIC_LEN] = {0};
    uint8_t payload[BC_MAX_PAYLOAD_LEN];
    ssize_t n;

    if (ensure_connected() != BC_OK || g_cb == NULL) {
        return BC_ERR_INVALID;
    }

    n = bc_read(g_fd, topic, sizeof(topic), payload, sizeof(payload), timeout_ms);
    if (n < 0) {
        return (int)n;
    }

    g_cb(topic, payload, (size_t)n, g_user);
    return BC_OK;
}
