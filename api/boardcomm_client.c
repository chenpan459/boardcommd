#include "boardcomm.h"

#include "ipc_protocol.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int g_fd = -1;
static boardcomm_msg_cb g_cb;
static void *g_user;

static int write_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);

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
    return boardcomm_init(NULL);
}

int boardcomm_init(const char *socket_path)
{
    struct sockaddr_un addr;
    const char *path = socket_path != NULL ? socket_path : BC_DEFAULT_SOCKET_PATH;

    if (g_fd >= 0) {
        return BC_OK;
    }

    g_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (g_fd < 0) {
        return BC_ERR_IO;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (connect(g_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(g_fd);
        g_fd = -1;
        return BC_ERR_IO;
    }

    return BC_OK;
}

void boardcomm_shutdown(void)
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    g_cb = NULL;
    g_user = NULL;
}

static int send_ipc_message(uint16_t type, const char *topic, const void *payload, size_t len)
{
    bc_ipc_header_t header;
    size_t topic_len;

    if (ensure_connected() != BC_OK) {
        return BC_ERR_IO;
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

    memset(&header, 0, sizeof(header));
    header.magic = BC_IPC_MAGIC;
    header.version = 1;
    header.type = type;
    header.topic_len = (uint16_t)topic_len;
    header.payload_len = (uint32_t)len;

    if (write_all(g_fd, &header, sizeof(header)) != BC_OK ||
        write_all(g_fd, topic, topic_len) != BC_OK) {
        return BC_ERR_IO;
    }
    if (len > 0 && write_all(g_fd, payload, len) != BC_OK) {
        return BC_ERR_IO;
    }

    return BC_OK;
}

int boardcomm_publish(const char *topic, const void *payload, size_t len)
{
    return send_ipc_message(BC_IPC_PUBLISH, topic, payload, len);
}

int boardcomm_subscribe(const char *topic, boardcomm_msg_cb cb, void *user)
{
    int rc;

    if (cb == NULL) {
        return BC_ERR_INVALID;
    }

    rc = send_ipc_message(BC_IPC_SUBSCRIBE, topic, NULL, 0);
    if (rc == BC_OK) {
        g_cb = cb;
        g_user = user;
    }
    return rc;
}

int boardcomm_poll(int timeout_ms)
{
    bc_ipc_header_t header;
    char topic[BC_MAX_TOPIC_LEN] = {0};
    uint8_t payload[BC_MAX_PAYLOAD_LEN];

    (void)timeout_ms;
    if (ensure_connected() != BC_OK || g_cb == NULL) {
        return BC_ERR_INVALID;
    }

    if (read_all(g_fd, &header, sizeof(header)) != BC_OK ||
        header.magic != BC_IPC_MAGIC ||
        header.type != BC_IPC_DELIVER ||
        header.topic_len == 0 ||
        header.topic_len >= BC_MAX_TOPIC_LEN ||
        header.payload_len > BC_MAX_PAYLOAD_LEN) {
        return BC_ERR_IO;
    }

    if (read_all(g_fd, topic, header.topic_len) != BC_OK ||
        read_all(g_fd, payload, header.payload_len) != BC_OK) {
        return BC_ERR_IO;
    }
    topic[header.topic_len] = '\0';

    g_cb(topic, payload, header.payload_len, g_user);
    return BC_OK;
}
