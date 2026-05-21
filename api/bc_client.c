#include "bc.h"

#include "bc_shm.h"
#include "ipc_protocol.h"

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct bc_context {
    int fd;
    bc_shm_session_t shm;
    int shm_ready;
    bc_msg_cb cb;
    void *user;
};

static int g_fd = -1;
static bc_msg_cb g_cb;
static void *g_user;
static bc_shm_session_t g_shm;
static int g_shm_ready;

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

static int recv_fd(int fd, int *out_fd)
{
    struct msghdr msg;
    struct iovec iov;
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    char byte = 0;
    struct cmsghdr *cmsg;

    memset(&msg, 0, sizeof(msg));
    iov.iov_base = &byte;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    if (recvmsg(fd, &msg, 0) <= 0) {
        return BC_ERR_IO;
    }

    cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == NULL || cmsg->cmsg_type != SCM_RIGHTS) {
        return BC_ERR_IO;
    }
    memcpy(out_fd, CMSG_DATA(cmsg), sizeof(int));
    return BC_OK;
}

static int send_shm_kick(int fd)
{
    bc_ipc_header_t header;

    memset(&header, 0, sizeof(header));
    header.magic = BC_IPC_MAGIC;
    header.version = BC_IPC_VERSION;
    header.type = BC_IPC_SHM_KICK;
    return write_all(fd, &header, sizeof(header));
}

static int shm_write_frame(bc_shm_session_t *shm, int fd, const void *frame, size_t len)
{
    int rc;

    for (int i = 0; i < 10000; ++i) {
        rc = bc_shm_ring_write(shm->tx, frame, len);
        if (rc != BC_ERR_NOMEM) {
            break;
        }
        (void)send_shm_kick(fd);
        usleep(100);
    }
    if (rc != BC_OK) {
        return rc;
    }
    return send_shm_kick(fd);
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
    if (handle == g_fd) {
        if (g_shm_ready) {
            bc_shm_close(&g_shm);
            g_shm_ready = 0;
        }
        g_fd = -1;
    }
    return close(handle) == 0 ? BC_OK : BC_ERR_IO;
}

static int build_ipc_frame(
    uint8_t *frame,
    size_t frame_cap,
    size_t *frame_len,
    uint16_t type,
    const char *channel,
    const char *topic,
    const void *payload,
    size_t len,
    const bc_publish_opts_t *opts)
{
    bc_ipc_header_t header;
    size_t channel_len = 0;
    size_t topic_len;
    size_t total;

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
    header.version = BC_IPC_VERSION;
    header.type = type;
    header.topic_len = (uint16_t)topic_len;
    header.channel_len = (uint16_t)channel_len;
    header.payload_len = (uint32_t)len;
    if (opts != NULL) {
        header.dst_node = opts->dst_node;
        header.qos = opts->qos;
        header.flags = opts->flags;
    }

    total = sizeof(header) + channel_len + topic_len + len;
    if (total > frame_cap) {
        return BC_ERR_INVALID;
    }

    memcpy(frame, &header, sizeof(header));
    if (channel_len > 0) {
        memcpy(frame + sizeof(header), channel, channel_len);
    }
    memcpy(frame + sizeof(header) + channel_len, topic, topic_len);
    if (len > 0 && payload != NULL) {
        memcpy(frame + sizeof(header) + channel_len + topic_len, payload, len);
    }
    *frame_len = total;
    return BC_OK;
}

static int send_ipc_message(
    int handle,
    bc_shm_session_t *shm,
    int shm_ready,
    uint16_t type,
    const char *channel,
    const char *topic,
    const void *payload,
    size_t len,
    const bc_publish_opts_t *opts)
{
    uint8_t frame[sizeof(bc_ipc_header_t) + BC_MAX_CHANNEL_LEN + BC_MAX_TOPIC_LEN + BC_MAX_PAYLOAD_LEN];
    size_t frame_len;
    int rc;

    if (handle < 0) {
        return BC_ERR_INVALID;
    }

    rc = build_ipc_frame(frame, sizeof(frame), &frame_len, type, channel, topic, payload, len, opts);
    if (rc != BC_OK) {
        return rc;
    }

    if (shm_ready && shm != NULL && shm->tx != NULL) {
        return shm_write_frame(shm, handle, frame, frame_len);
    }

    return write_all(handle, frame, frame_len);
}

ssize_t bc_write(int handle, const char *topic, const void *payload, size_t len)
{
    bc_shm_session_t *shm = handle == g_fd && g_shm_ready ? &g_shm : NULL;
    int rc = send_ipc_message(
        handle,
        shm,
        shm != NULL,
        BC_IPC_PUBLISH,
        NULL,
        topic,
        payload,
        len,
        NULL);

    return rc == BC_OK ? (ssize_t)len : (ssize_t)rc;
}

ssize_t bc_write_channel(
    int handle,
    const char *channel,
    const char *topic,
    const void *payload,
    size_t len)
{
    return bc_write_ex(handle, channel, topic, payload, len, NULL);
}

ssize_t bc_write_ex(
    int handle,
    const char *channel,
    const char *topic,
    const void *payload,
    size_t len,
    const bc_publish_opts_t *opts)
{
    bc_shm_session_t *shm = handle == g_fd && g_shm_ready ? &g_shm : NULL;
    int rc = send_ipc_message(
        handle,
        shm,
        shm != NULL,
        BC_IPC_PUBLISH,
        channel,
        topic,
        payload,
        len,
        opts);

    return rc == BC_OK ? (ssize_t)len : (ssize_t)rc;
}

int bc_subscribe_fd(int handle, const char *topic)
{
    bc_shm_session_t *shm = handle == g_fd && g_shm_ready ? &g_shm : NULL;
    return send_ipc_message(handle, shm, shm != NULL, BC_IPC_SUBSCRIBE, NULL, topic, NULL, 0, NULL);
}

static ssize_t read_ipc_deliver(
    int handle,
    bc_shm_session_t *shm,
    int shm_ready,
    char *topic,
    size_t topic_cap,
    void *payload,
    size_t payload_cap,
    int timeout_ms)
{
    struct pollfd pfd;
    bc_ipc_header_t header;
    char channel[BC_MAX_CHANNEL_LEN];
    size_t frame_len;
    size_t payload_len;
    uint8_t frame[sizeof(bc_ipc_header_t) + BC_MAX_CHANNEL_LEN + BC_MAX_TOPIC_LEN + BC_MAX_PAYLOAD_LEN];
    int rc;

    if (handle < 0 || topic == NULL || topic_cap == 0 || payload == NULL) {
        return BC_ERR_INVALID;
    }

    if (shm_ready && shm != NULL && shm->rx != NULL) {
        rc = bc_shm_ring_read(shm->rx, frame, sizeof(frame), &frame_len);
        if (rc == BC_OK) {
            goto parse_frame;
        }
        if (rc != BC_ERR_NOT_FOUND) {
            return rc;
        }
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

    if (read_all(handle, &header, sizeof(header)) != BC_OK) {
        return BC_ERR_IO;
    }
    if (header.magic != BC_IPC_MAGIC) {
        return BC_ERR_IO;
    }
    if (header.type == BC_IPC_STATS) {
        if (read_all(handle, payload, header.payload_len) != BC_OK) {
            return BC_ERR_IO;
        }
        return (ssize_t)header.payload_len;
    }
    if (header.type != BC_IPC_DELIVER) {
        return BC_ERR_IO;
    }

    if (header.channel_len >= BC_MAX_CHANNEL_LEN ||
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

parse_frame:
    if (frame_len < bc_ipc_header_size(BC_IPC_VERSION)) {
        return BC_ERR_IO;
    }
    memcpy(&header, frame, sizeof(header));
    if (header.magic != BC_IPC_MAGIC || header.type != BC_IPC_DELIVER) {
        return BC_ERR_IO;
    }
    if (header.topic_len == 0 || header.topic_len >= BC_MAX_TOPIC_LEN ||
        header.payload_len > BC_MAX_PAYLOAD_LEN ||
        topic_cap <= header.topic_len ||
        payload_cap < header.payload_len) {
        return BC_ERR_IO;
    }
    memcpy(topic, frame + bc_ipc_header_size(header.version) + header.channel_len, header.topic_len);
    topic[header.topic_len] = '\0';
    payload_len = header.payload_len;
    memcpy(
        payload,
        frame + bc_ipc_header_size(header.version) + header.channel_len + header.topic_len,
        payload_len);
    return (ssize_t)payload_len;
}

ssize_t bc_read(
    int handle,
    char *topic,
    size_t topic_cap,
    void *payload,
    size_t payload_cap,
    int timeout_ms)
{
    bc_shm_session_t *shm = handle == g_fd && g_shm_ready ? &g_shm : NULL;
    return read_ipc_deliver(handle, shm, shm != NULL, topic, topic_cap, payload, payload_cap, timeout_ms);
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
    if (g_shm_ready) {
        bc_shm_close(&g_shm);
        g_shm_ready = 0;
    }
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

bc_context_t *bc_context_create(const char *socket_path)
{
    bc_context_t *ctx = calloc(1, sizeof(*ctx));

    if (ctx == NULL) {
        return NULL;
    }
    ctx->fd = bc_open(socket_path);
    if (ctx->fd < 0) {
        free(ctx);
        return NULL;
    }
    return ctx;
}

void bc_context_destroy(bc_context_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    if (ctx->shm_ready) {
        bc_shm_close(&ctx->shm);
    }
    if (ctx->fd >= 0) {
        close(ctx->fd);
    }
    free(ctx);
}

int bc_context_publish(
    bc_context_t *ctx,
    const char *topic,
    const void *payload,
    size_t len)
{
    if (ctx == NULL) {
        return BC_ERR_INVALID;
    }
    return send_ipc_message(
        ctx->fd,
        ctx->shm_ready ? &ctx->shm : NULL,
        ctx->shm_ready,
        BC_IPC_PUBLISH,
        NULL,
        topic,
        payload,
        len,
        NULL) == BC_OK
        ? BC_OK
        : BC_ERR_IO;
}

int bc_context_subscribe(bc_context_t *ctx, const char *topic, bc_msg_cb cb, void *user)
{
    if (ctx == NULL || cb == NULL) {
        return BC_ERR_INVALID;
    }
    if (send_ipc_message(
            ctx->fd,
            ctx->shm_ready ? &ctx->shm : NULL,
            ctx->shm_ready,
            BC_IPC_SUBSCRIBE,
            NULL,
            topic,
            NULL,
            0,
            NULL) != BC_OK) {
        return BC_ERR_IO;
    }
    ctx->cb = cb;
    ctx->user = user;
    return BC_OK;
}

int bc_context_poll(bc_context_t *ctx, int timeout_ms)
{
    char topic[BC_MAX_TOPIC_LEN] = {0};
    uint8_t payload[BC_MAX_PAYLOAD_LEN];
    ssize_t n;

    if (ctx == NULL || ctx->cb == NULL) {
        return BC_ERR_INVALID;
    }

    n = read_ipc_deliver(
        ctx->fd,
        ctx->shm_ready ? &ctx->shm : NULL,
        ctx->shm_ready,
        topic,
        sizeof(topic),
        payload,
        sizeof(payload),
        timeout_ms);
    if (n < 0) {
        return (int)n;
    }

    ctx->cb(topic, payload, (size_t)n, ctx->user);
    return BC_OK;
}

int bc_enable_shm(int handle)
{
    bc_ipc_header_t header;
    int shm_fd = -1;

    if (handle < 0) {
        return BC_ERR_INVALID;
    }
    if (handle != g_fd) {
        return BC_ERR_INVALID;
    }

    memset(&header, 0, sizeof(header));
    header.magic = BC_IPC_MAGIC;
    header.version = BC_IPC_VERSION;
    header.type = BC_IPC_SHM_SETUP;
    if (write_all(handle, &header, sizeof(header)) != BC_OK) {
        return BC_ERR_IO;
    }
    if (recv_fd(handle, &shm_fd) != BC_OK) {
        return BC_ERR_IO;
    }
    if (g_shm_ready) {
        bc_shm_close(&g_shm);
        g_shm_ready = 0;
    }
    if (bc_shm_attach(shm_fd, &g_shm, 1) != BC_OK) {
        close(shm_fd);
        return BC_ERR_IO;
    }
    close(shm_fd);
    g_shm_ready = 1;
    return BC_OK;
}

int bc_context_enable_shm(bc_context_t *ctx)
{
    bc_ipc_header_t header;
    int shm_fd = -1;

    if (ctx == NULL || ctx->fd < 0) {
        return BC_ERR_INVALID;
    }

    memset(&header, 0, sizeof(header));
    header.magic = BC_IPC_MAGIC;
    header.version = BC_IPC_VERSION;
    header.type = BC_IPC_SHM_SETUP;
    if (write_all(ctx->fd, &header, sizeof(header)) != BC_OK) {
        return BC_ERR_IO;
    }
    if (recv_fd(ctx->fd, &shm_fd) != BC_OK) {
        return BC_ERR_IO;
    }
    if (ctx->shm_ready) {
        bc_shm_close(&ctx->shm);
        ctx->shm_ready = 0;
    }
    if (bc_shm_attach(shm_fd, &ctx->shm, 1) != BC_OK) {
        close(shm_fd);
        return BC_ERR_IO;
    }
    close(shm_fd);
    ctx->shm_ready = 1;
    return BC_OK;
}

int bc_get_stats(int handle, bc_client_stats_t *stats)
{
    bc_ipc_header_t header;

    if (handle < 0 || stats == NULL) {
        return BC_ERR_INVALID;
    }

    memset(&header, 0, sizeof(header));
    header.magic = BC_IPC_MAGIC;
    header.version = BC_IPC_VERSION;
    header.type = BC_IPC_STATS;
    if (write_all(handle, &header, sizeof(header)) != BC_OK) {
        return BC_ERR_IO;
    }
    if (read_all(handle, stats, sizeof(*stats)) != BC_OK) {
        return BC_ERR_IO;
    }
    return BC_OK;
}

int bc_discover_nodes(int handle, uint32_t *nodes, size_t cap, size_t *count)
{
    (void)handle;
    (void)nodes;
    if (count != NULL) {
        *count = 0;
    }
    (void)cap;
    return BC_ERR_NOT_FOUND;
}

int bc_persist_enable(int handle, const char *queue_path)
{
    (void)handle;
    (void)queue_path;
    return BC_OK;
}
