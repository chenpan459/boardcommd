#include "boardcomm.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t type;
    uint8_t reserved;
    uint32_t value;
} bc_file_pkt_hdr_t;

static uint32_t bc_file_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t mask = 0u - (crc & 1u);

            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return crc;
}

static const char *basename_only(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash != NULL ? slash + 1 : path;
}

static ssize_t send_file_packet(
    int handle,
    const char *channel,
    const char *topic,
    uint8_t type,
    uint32_t value,
    const void *extra,
    size_t extra_len)
{
    uint8_t payload[BC_MAX_PAYLOAD_LEN];
    bc_file_pkt_hdr_t hdr;
    size_t total;

    if (extra_len > BC_MAX_PAYLOAD_LEN - sizeof(hdr)) {
        return BC_ERR_INVALID;
    }

    hdr.magic = BC_FILE_PKT_MAGIC;
    hdr.type = type;
    hdr.reserved = 0;
    hdr.value = value;
    total = sizeof(hdr);
    memcpy(payload, &hdr, sizeof(hdr));
    if (extra_len > 0) {
        memcpy(payload + sizeof(hdr), extra, extra_len);
        total += extra_len;
    }

    if (channel != NULL && channel[0] != '\0') {
        return bc_write_channel(handle, channel, topic, payload, total);
    }
    return bc_write(handle, topic, payload, total);
}

static int parse_file_packet(
    const void *payload,
    size_t len,
    bc_file_pkt_hdr_t *hdr,
    const uint8_t **extra,
    size_t *extra_len)
{
    if (len < sizeof(*hdr)) {
        return BC_ERR_INVALID;
    }

    memcpy(hdr, payload, sizeof(*hdr));
    if (hdr->magic != BC_FILE_PKT_MAGIC) {
        return BC_ERR_INVALID;
    }

    *extra = (const uint8_t *)payload + sizeof(*hdr);
    *extra_len = len - sizeof(*hdr);
    return BC_OK;
}

static int send_file_path(
    int handle,
    const char *channel,
    const char *topic,
    const char *path)
{
    struct stat st;
    FILE *fp;
    uint8_t chunk[BC_FILE_CHUNK_DATA_MAX];
    uint32_t crc;
    uint32_t seq = 0;
    size_t name_len;
    const char *name;
    ssize_t n;

    if (handle < 0 || topic == NULL || topic[0] == '\0' || path == NULL || path[0] == '\0') {
        return BC_ERR_INVALID;
    }

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return BC_ERR_IO;
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) {
        return BC_ERR_INVALID;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return BC_ERR_IO;
    }

    uint8_t start_meta[BC_MAX_PAYLOAD_LEN - sizeof(bc_file_pkt_hdr_t)];
    size_t channel_len = 0;
    size_t meta_len;

    name = basename_only(path);
    name_len = strlen(name);
    if (name_len == 0 || name_len > BC_FILE_NAME_MAX) {
        fclose(fp);
        return BC_ERR_INVALID;
    }
    if (channel != NULL && channel[0] != '\0') {
        channel_len = strlen(channel);
        if (channel_len == 0 || channel_len >= BC_MAX_CHANNEL_LEN) {
            fclose(fp);
            return BC_ERR_INVALID;
        }
    }

    if (sizeof(uint16_t) * 2u + name_len + channel_len > sizeof(start_meta)) {
        fclose(fp);
        return BC_ERR_INVALID;
    }

    meta_len = 0;
    memcpy(start_meta + meta_len, &name_len, sizeof(uint16_t));
    meta_len += sizeof(uint16_t);
    memcpy(start_meta + meta_len, name, name_len);
    meta_len += name_len;
    memcpy(start_meta + meta_len, &channel_len, sizeof(uint16_t));
    meta_len += sizeof(uint16_t);
    if (channel_len > 0) {
        memcpy(start_meta + meta_len, channel, channel_len);
        meta_len += channel_len;
    }

    n = send_file_packet(
        handle,
        channel,
        topic,
        BC_FILE_PKT_START,
        (uint32_t)st.st_size,
        start_meta,
        meta_len);
    if (n < 0) {
        fclose(fp);
        return (int)n;
    }

    crc = 0xffffffffu;
    while ((n = (ssize_t)fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        crc = bc_file_crc32_update(crc, chunk, (size_t)n);

        n = send_file_packet(
            handle,
            channel,
            topic,
            BC_FILE_PKT_DATA,
            seq,
            chunk,
            (size_t)n);
        if (n < 0) {
            fclose(fp);
            return (int)n;
        }
        seq++;
    }

    if (ferror(fp)) {
        fclose(fp);
        return BC_ERR_IO;
    }
    fclose(fp);

    n = send_file_packet(handle, channel, topic, BC_FILE_PKT_END, ~crc, NULL, 0);
    return n < 0 ? (int)n : BC_OK;
}

static int parse_start_meta(
    const uint8_t *extra,
    size_t extra_len,
    const char *expect_channel)
{
    uint16_t name_len;
    uint16_t channel_len;
    size_t off = 0;

    if (extra_len < sizeof(uint16_t) * 2u) {
        return BC_ERR_INVALID;
    }

    memcpy(&name_len, extra + off, sizeof(name_len));
    off += sizeof(name_len);
    if (name_len == 0 || name_len > BC_FILE_NAME_MAX || off + name_len + sizeof(uint16_t) > extra_len) {
        return BC_ERR_INVALID;
    }
    off += name_len;

    memcpy(&channel_len, extra + off, sizeof(channel_len));
    off += sizeof(channel_len);
    if (channel_len >= BC_MAX_CHANNEL_LEN || off + channel_len != extra_len) {
        return BC_ERR_INVALID;
    }

    if (expect_channel != NULL && expect_channel[0] != '\0') {
        size_t expect_len = strlen(expect_channel);

        if (channel_len != expect_len ||
            memcmp(extra + off, expect_channel, channel_len) != 0) {
            return BC_ERR_INVALID;
        }
    } else if (channel_len != 0) {
        return BC_ERR_INVALID;
    }

    return BC_OK;
}

static int recv_file_path(
    int handle,
    const char *channel,
    const char *topic,
    const char *path,
    int timeout_ms)
{
    char rx_topic[BC_MAX_TOPIC_LEN];
    uint8_t payload[BC_MAX_PAYLOAD_LEN];
    bc_file_pkt_hdr_t hdr;
    const uint8_t *extra;
    size_t extra_len;
    FILE *fp = NULL;
    uint32_t expected_size = 0;
    uint32_t received = 0;
    uint32_t expected_seq = 0;
    uint32_t crc = 0xffffffffu;
    int got_start = 0;
    int got_end = 0;
    ssize_t n;
    int rc;

    if (handle < 0 || topic == NULL || topic[0] == '\0' || path == NULL || path[0] == '\0') {
        return BC_ERR_INVALID;
    }

    rc = bc_subscribe_fd(handle, topic);
    if (rc != BC_OK) {
        return rc;
    }

    for (;;) {
        n = bc_read(handle, rx_topic, sizeof(rx_topic), payload, sizeof(payload), timeout_ms);
        if (n == BC_ERR_TIMEOUT || n == BC_ERR_NOT_FOUND) {
            return got_end ? BC_OK : BC_ERR_TIMEOUT;
        }
        if (n < 0) {
            if (fp != NULL) {
                fclose(fp);
                (void)unlink(path);
            }
            return (int)n;
        }
        if (strcmp(rx_topic, topic) != 0) {
            continue;
        }

        rc = parse_file_packet(payload, (size_t)n, &hdr, &extra, &extra_len);
        if (rc != BC_OK) {
            continue;
        }

        if (hdr.type == BC_FILE_PKT_START) {
            if (got_start || parse_start_meta(extra, extra_len, channel) != BC_OK) {
                continue;
            }
            expected_size = hdr.value;
            fp = fopen(path, "wb");
            if (fp == NULL) {
                return BC_ERR_IO;
            }
            got_start = 1;
            continue;
        }

        if (!got_start || fp == NULL) {
            continue;
        }

        if (hdr.type == BC_FILE_PKT_DATA) {
            if (hdr.value != expected_seq || extra_len == 0) {
                fclose(fp);
                fp = NULL;
                (void)unlink(path);
                return BC_ERR_IO; /* likely dropped chunks (send too fast) */
            }

            if (fwrite(extra, 1, extra_len, fp) != extra_len) {
                fclose(fp);
                fp = NULL;
                (void)unlink(path);
                return BC_ERR_IO;
            }

            crc = bc_file_crc32_update(crc, extra, extra_len);
            received += (uint32_t)extra_len;
            expected_seq++;
            continue;
        }

        if (hdr.type == BC_FILE_PKT_END) {
            if (got_end) {
                continue;
            }
            fclose(fp);
            fp = NULL;
            got_end = 1;

            if (received != expected_size || hdr.value != ~crc) {
                (void)unlink(path);
                return BC_ERR_IO;
            }
            return BC_OK;
        }
    }
}

int bc_file_send(int handle, const char *topic, const char *path)
{
    return send_file_path(handle, NULL, topic, path);
}

int bc_file_send_channel(
    int handle,
    const char *channel,
    const char *topic,
    const char *path)
{
    return send_file_path(handle, channel, topic, path);
}

int bc_file_recv(
    int handle,
    const char *topic,
    const char *path,
    int timeout_ms)
{
    return recv_file_path(handle, NULL, topic, path, timeout_ms);
}

int bc_file_recv_channel(
    int handle,
    const char *channel,
    const char *topic,
    const char *path,
    int timeout_ms)
{
    return recv_file_path(handle, channel, topic, path, timeout_ms);
}
