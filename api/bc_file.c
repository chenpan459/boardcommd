#include "bc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BC_FILE_SEND_RETRY_MAX 64
#define BC_FILE_DATA_PACE_US 500
#define BC_FILE_PKT_FLAG_V2 0x01u
#define BC_FILE_META_V2_VERSION 2u
#define BC_FILE_PKT_NACK 4u
#define BC_FILE_PKT_DONE 5u
#define BC_FILE_DATA_BODY_MAX (BC_FILE_CHUNK_DATA_MAX - sizeof(uint32_t))
#define BC_FILE_RETX_WINDOW 256u
#define BC_FILE_CTRL_READ_TIMEOUT_MS 20
#define BC_FILE_END_WAIT_ROUNDS 200

typedef struct {
    uint8_t valid;
    uint32_t seq;
    size_t len;
    uint8_t data[sizeof(uint32_t) + BC_FILE_DATA_BODY_MAX];
} bc_file_cache_slot_t;

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

static uint32_t make_transfer_id(const char *topic, const char *path, uint32_t size_hint)
{
    uint32_t seed = 0xffffffffu;
    uint32_t now = (uint32_t)time(NULL);
    uint32_t pid = (uint32_t)getpid();

    seed = bc_file_crc32_update(seed, (const uint8_t *)&now, sizeof(now));
    seed = bc_file_crc32_update(seed, (const uint8_t *)&pid, sizeof(pid));
    seed = bc_file_crc32_update(seed, (const uint8_t *)&size_hint, sizeof(size_hint));
    if (topic != NULL) {
        seed = bc_file_crc32_update(seed, (const uint8_t *)topic, strlen(topic));
    }
    if (path != NULL) {
        seed = bc_file_crc32_update(seed, (const uint8_t *)path, strlen(path));
    }
    return ~seed;
}

static ssize_t send_file_packet(
    int handle,
    const char *channel,
    const char *topic,
    uint8_t type,
    uint8_t reserved,
    uint32_t value,
    const void *extra,
    size_t extra_len)
{
    uint8_t payload[BC_MAX_PAYLOAD_LEN];
    bc_file_pkt_hdr_t hdr;
    bc_publish_opts_t opts = {
        .dst_node = 0,
        .qos = BC_QOS_AT_LEAST_ONCE,
        .flags = 0,
    };
    size_t total;
    int attempt;

    if (extra_len > BC_MAX_PAYLOAD_LEN - sizeof(hdr)) {
        return BC_ERR_INVALID;
    }

    hdr.magic = BC_FILE_PKT_MAGIC;
    hdr.type = type;
    hdr.reserved = reserved;
    hdr.value = value;
    total = sizeof(hdr);
    memcpy(payload, &hdr, sizeof(hdr));
    if (extra_len > 0) {
        memcpy(payload + sizeof(hdr), extra, extra_len);
        total += extra_len;
    }

    for (attempt = 0; attempt < BC_FILE_SEND_RETRY_MAX; ++attempt) {
        ssize_t n = bc_write_ex(handle, channel, topic, payload, total, &opts);

        if (n >= 0) {
            if (type == BC_FILE_PKT_DATA && BC_FILE_DATA_PACE_US > 0) {
                usleep(BC_FILE_DATA_PACE_US);
            }
            return n;
        }
        if (n == BC_ERR_NOMEM || n == BC_ERR_IO) {
            usleep(1000 * (useconds_t)(attempt + 1));
            continue;
        }
        return n;
    }

    return BC_ERR_IO;
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

static int send_file_v2_control(
    int handle,
    const char *channel,
    const char *topic,
    uint8_t type,
    uint32_t value,
    uint32_t transfer_id)
{
    return (int)send_file_packet(
        handle,
        channel,
        topic,
        type,
        BC_FILE_PKT_FLAG_V2,
        value,
        &transfer_id,
        sizeof(transfer_id));
}

static int parse_file_v2_control(
    const bc_file_pkt_hdr_t *hdr,
    const uint8_t *extra,
    size_t extra_len,
    uint8_t expect_type,
    uint32_t expect_transfer_id)
{
    uint32_t transfer_id = 0;

    if (hdr == NULL || extra == NULL) {
        return BC_ERR_INVALID;
    }
    if (hdr->type != expect_type || (hdr->reserved & BC_FILE_PKT_FLAG_V2) == 0 || extra_len != sizeof(uint32_t)) {
        return BC_ERR_INVALID;
    }
    memcpy(&transfer_id, extra, sizeof(transfer_id));
    return transfer_id == expect_transfer_id ? BC_OK : BC_ERR_NOT_FOUND;
}

static bc_file_cache_slot_t *find_cache_slot(
    bc_file_cache_slot_t *cache,
    size_t cache_len,
    uint32_t seq)
{
    for (size_t i = 0; i < cache_len; ++i) {
        if (cache[i].valid != 0 && cache[i].seq == seq) {
            return &cache[i];
        }
    }
    return NULL;
}

static void store_cache_slot(
    bc_file_cache_slot_t *cache,
    size_t cache_len,
    uint32_t seq,
    const uint8_t *data,
    size_t len)
{
    size_t idx = (size_t)(seq % (uint32_t)cache_len);

    cache[idx].valid = 1;
    cache[idx].seq = seq;
    cache[idx].len = len;
    memcpy(cache[idx].data, data, len);
}

static int sender_handle_controls(
    int handle,
    const char *channel,
    const char *topic,
    uint32_t transfer_id,
    uint32_t seq_sent,
    bc_file_cache_slot_t *cache,
    size_t cache_len,
    int timeout_ms,
    int *done_seen,
    int *resent_any)
{
    char rx_topic[BC_MAX_TOPIC_LEN];
    uint8_t payload[BC_MAX_PAYLOAD_LEN];
    bc_file_pkt_hdr_t hdr;
    const uint8_t *extra;
    size_t extra_len;
    ssize_t n;
    int rc;

    if (done_seen != NULL) {
        *done_seen = 0;
    }
    if (resent_any != NULL) {
        *resent_any = 0;
    }

    for (;;) {
        n = bc_read(handle, rx_topic, sizeof(rx_topic), payload, sizeof(payload), timeout_ms);
        if (n == BC_ERR_TIMEOUT || n == BC_ERR_NOT_FOUND) {
            return BC_OK;
        }
        if (n < 0) {
            return (int)n;
        }
        timeout_ms = 0;
        if (strcmp(rx_topic, topic) != 0) {
            continue;
        }

        rc = parse_file_packet(payload, (size_t)n, &hdr, &extra, &extra_len);
        if (rc != BC_OK) {
            continue;
        }

        if (parse_file_v2_control(&hdr, extra, extra_len, BC_FILE_PKT_DONE, transfer_id) == BC_OK) {
            if (done_seen != NULL) {
                *done_seen = 1;
            }
            continue;
        }

        if (parse_file_v2_control(&hdr, extra, extra_len, BC_FILE_PKT_NACK, transfer_id) == BC_OK) {
            bc_file_cache_slot_t *slot;

            if (hdr.value >= seq_sent) {
                continue;
            }
            slot = find_cache_slot(cache, cache_len, hdr.value);
            if (slot == NULL) {
                fprintf(
                    stderr,
                    "boardcomm file_send: cannot retransmit seq=%u transfer=0x%08x (out of cache window)\n",
                    hdr.value,
                    transfer_id);
                return BC_ERR_IO;
            }
            rc = (int)send_file_packet(
                handle,
                channel,
                topic,
                BC_FILE_PKT_DATA,
                BC_FILE_PKT_FLAG_V2,
                slot->seq,
                slot->data,
                slot->len);
            if (rc < 0) {
                return rc;
            }
            if (resent_any != NULL) {
                *resent_any = 1;
            }
        }
    }
}

static int send_file_path(
    int handle,
    const char *channel,
    const char *topic,
    const char *path)
{
    struct stat st;
    FILE *fp;
    uint8_t chunk[sizeof(uint32_t) + BC_FILE_DATA_BODY_MAX];
    uint32_t crc;
    uint32_t seq = 0;
    uint32_t transfer_id;
    bc_file_cache_slot_t cache[BC_FILE_RETX_WINDOW];
    size_t name_len;
    const char *name;
    ssize_t n;
    int rc;
    int done_seen;
    int resent_any;

    if (handle < 0 || topic == NULL || topic[0] == '\0' || path == NULL || path[0] == '\0') {
        return BC_ERR_INVALID;
    }
    memset(cache, 0, sizeof(cache));

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

    transfer_id = make_transfer_id(topic, path, (uint32_t)st.st_size);
    rc = bc_subscribe_fd(handle, topic);
    if (rc != BC_OK) {
        fclose(fp);
        return rc;
    }
    meta_len = 0;
    start_meta[meta_len++] = BC_FILE_META_V2_VERSION;
    memcpy(start_meta + meta_len, &transfer_id, sizeof(transfer_id));
    meta_len += sizeof(transfer_id);
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
        BC_FILE_PKT_FLAG_V2,
        (uint32_t)st.st_size,
        start_meta,
        meta_len);
    if (n < 0) {
        fclose(fp);
        return (int)n;
    }

    crc = 0xffffffffu;
    while ((n = (ssize_t)fread(chunk + sizeof(transfer_id), 1, BC_FILE_DATA_BODY_MAX, fp)) > 0) {
        size_t data_len = (size_t)n;

        memcpy(chunk, &transfer_id, sizeof(transfer_id));
        crc = bc_file_crc32_update(crc, chunk + sizeof(transfer_id), data_len);

        n = send_file_packet(
            handle,
            channel,
            topic,
            BC_FILE_PKT_DATA,
            BC_FILE_PKT_FLAG_V2,
            seq,
            chunk,
            sizeof(transfer_id) + data_len);
        if (n < 0) {
            fclose(fp);
            return (int)n;
        }
        store_cache_slot(cache, BC_FILE_RETX_WINDOW, seq, chunk, sizeof(transfer_id) + data_len);
        rc = sender_handle_controls(
            handle,
            channel,
            topic,
            transfer_id,
            seq + 1u,
            cache,
            BC_FILE_RETX_WINDOW,
            0,
            NULL,
            NULL);
        if (rc != BC_OK) {
            fclose(fp);
            return rc;
        }
        seq++;
    }

    if (ferror(fp)) {
        fclose(fp);
        return BC_ERR_IO;
    }
    fclose(fp);

    rc = (int)send_file_packet(
        handle,
        channel,
        topic,
        BC_FILE_PKT_END,
        BC_FILE_PKT_FLAG_V2,
        ~crc,
        &transfer_id,
        sizeof(transfer_id));
    if (rc < 0) {
        return rc;
    }

    for (int round = 0; round < BC_FILE_END_WAIT_ROUNDS; ++round) {
        rc = sender_handle_controls(
            handle,
            channel,
            topic,
            transfer_id,
            seq,
            cache,
            BC_FILE_RETX_WINDOW,
            BC_FILE_CTRL_READ_TIMEOUT_MS,
            &done_seen,
            &resent_any);
        if (rc != BC_OK) {
            return rc;
        }
        if (done_seen != 0) {
            return BC_OK;
        }
        if (resent_any != 0) {
            rc = send_file_v2_control(handle, channel, topic, BC_FILE_PKT_END, ~crc, transfer_id);
            if (rc < 0) {
                return rc;
            }
        }
    }

    fprintf(
        stderr,
        "boardcomm file_send: timeout waiting DONE transfer=0x%08x topic=%s\n",
        transfer_id,
        topic);
    return BC_ERR_TIMEOUT;
}

static int parse_start_meta(
    const uint8_t *extra,
    size_t extra_len,
    const char *expect_channel,
    uint8_t expect_v2,
    uint32_t *transfer_id)
{
    uint16_t name_len;
    uint16_t channel_len;
    size_t off = 0;

    if (expect_v2 != 0) {
        if (extra_len < 1u + sizeof(uint32_t) + sizeof(uint16_t) * 2u || extra[0] != BC_FILE_META_V2_VERSION) {
            return BC_ERR_INVALID;
        }
        off = 1;
        memcpy(transfer_id, extra + off, sizeof(uint32_t));
        off += sizeof(uint32_t);
    } else if (extra_len < sizeof(uint16_t) * 2u) {
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
    uint32_t transfer_id = 0;
    uint32_t crc = 0xffffffffu;
    const uint8_t *data_ptr;
    size_t data_len;
    uint32_t pkt_transfer_id;
    uint32_t end_crc = 0;
    int got_start = 0;
    int got_end = 0;
    int v2_mode = 0;
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
            if (!got_start) {
                return BC_ERR_TIMEOUT;
            }
            fprintf(
                stderr,
                "boardcomm file_recv: idle timeout during transfer topic=%s received=%u/%u expect_seq=%u\n",
                topic,
                received,
                expected_size,
                expected_seq);
            if (fp != NULL) {
                fclose(fp);
                fp = NULL;
            }
            (void)unlink(path);
            if (v2_mode != 0) {
                (void)send_file_v2_control(
                    handle,
                    channel,
                    topic,
                    BC_FILE_PKT_NACK,
                    expected_seq,
                    transfer_id);
            }
            return BC_ERR_IO;
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
            fprintf(stderr, "boardcomm file_recv: ignore invalid packet topic=%s len=%zd\n", topic, n);
            continue;
        }

        if (hdr.type == BC_FILE_PKT_START) {
            int meta_v2 = (hdr.reserved & BC_FILE_PKT_FLAG_V2) != 0;

            if (got_start) {
                fprintf(stderr, "boardcomm file_recv: ignore duplicated START topic=%s\n", topic);
                continue;
            }
            if (parse_start_meta(extra, extra_len, channel, (uint8_t)meta_v2, &transfer_id) != BC_OK) {
                fprintf(stderr, "boardcomm file_recv: ignore START meta mismatch topic=%s\n", topic);
                continue;
            }
            expected_size = hdr.value;
            fp = fopen(path, "wb");
            if (fp == NULL) {
                return BC_ERR_IO;
            }
            got_start = 1;
            v2_mode = meta_v2;
            continue;
        }

        if (!got_start || fp == NULL) {
            continue;
        }

        if (hdr.type == BC_FILE_PKT_DATA) {
            data_ptr = extra;
            data_len = extra_len;
            if (v2_mode) {
                if ((hdr.reserved & BC_FILE_PKT_FLAG_V2) == 0 || extra_len <= sizeof(uint32_t)) {
                    fprintf(stderr, "boardcomm file_recv: ignore invalid v2 DATA topic=%s\n", topic);
                    continue;
                }
                memcpy(&pkt_transfer_id, extra, sizeof(pkt_transfer_id));
                if (pkt_transfer_id != transfer_id) {
                    fprintf(
                        stderr,
                        "boardcomm file_recv: ignore DATA transfer_id mismatch got=0x%08x expect=0x%08x\n",
                        pkt_transfer_id,
                        transfer_id);
                    continue;
                }
                data_ptr = extra + sizeof(uint32_t);
                data_len = extra_len - sizeof(uint32_t);
            }
            if (data_len == 0) {
                fprintf(stderr, "boardcomm file_recv: ignore empty DATA chunk topic=%s\n", topic);
                continue;
            }
            if (hdr.value < expected_seq) {
                fprintf(
                    stderr,
                    "boardcomm file_recv: ignore duplicated chunk seq=%u expect=%u\n",
                    hdr.value,
                    expected_seq);
                continue;
            }
            if (hdr.value > expected_seq) {
                fprintf(
                    stderr,
                    "boardcomm file_recv: out-of-order chunk got=%u expect=%u, waiting missing chunks\n",
                    hdr.value,
                    expected_seq);
                if (v2_mode != 0) {
                    (void)send_file_v2_control(
                        handle,
                        channel,
                        topic,
                        BC_FILE_PKT_NACK,
                        expected_seq,
                        transfer_id);
                }
                continue;
            }

            if (fwrite(data_ptr, 1, data_len, fp) != data_len) {
                fclose(fp);
                fp = NULL;
                (void)unlink(path);
                return BC_ERR_IO;
            }

            crc = bc_file_crc32_update(crc, data_ptr, data_len);
            received += (uint32_t)data_len;
            expected_seq++;
            if (got_end != 0 && received == expected_size) {
                fclose(fp);
                fp = NULL;
                if (end_crc != ~crc) {
                    fprintf(
                        stderr,
                        "boardcomm file_recv: final crc mismatch crc=0x%08x/0x%08x\n",
                        end_crc,
                        ~crc);
                    (void)unlink(path);
                    return BC_ERR_IO;
                }
                if (v2_mode != 0) {
                    (void)send_file_v2_control(
                        handle,
                        channel,
                        topic,
                        BC_FILE_PKT_DONE,
                        expected_seq,
                        transfer_id);
                }
                return BC_OK;
            }
            continue;
        }

        if (hdr.type == BC_FILE_PKT_END) {
            if (got_end) {
                continue;
            }
            got_end = 1;
            end_crc = hdr.value;
            if (v2_mode) {
                if ((hdr.reserved & BC_FILE_PKT_FLAG_V2) == 0 || extra_len != sizeof(uint32_t)) {
                    fprintf(stderr, "boardcomm file_recv: invalid v2 END packet\n");
                    (void)unlink(path);
                    return BC_ERR_IO;
                }
                memcpy(&pkt_transfer_id, extra, sizeof(pkt_transfer_id));
                if (pkt_transfer_id != transfer_id) {
                    fprintf(
                        stderr,
                        "boardcomm file_recv: END transfer_id mismatch got=0x%08x expect=0x%08x\n",
                        pkt_transfer_id,
                        transfer_id);
                    (void)unlink(path);
                    return BC_ERR_IO;
                }
            }

            if (received != expected_size) {
                fprintf(
                    stderr,
                    "boardcomm file_recv: END arrived early size=%u/%u, request retransmit seq=%u\n",
                    received,
                    expected_size,
                    expected_seq);
                if (v2_mode != 0) {
                    (void)send_file_v2_control(
                        handle,
                        channel,
                        topic,
                        BC_FILE_PKT_NACK,
                        expected_seq,
                        transfer_id);
                }
                continue;
            }
            fclose(fp);
            fp = NULL;
            if (end_crc != ~crc) {
                fprintf(
                    stderr,
                    "boardcomm file_recv: end check failed size=%u/%u crc=0x%08x/0x%08x\n",
                    received,
                    expected_size,
                    end_crc,
                    ~crc);
                (void)unlink(path);
                return BC_ERR_IO;
            }
            if (v2_mode != 0) {
                (void)send_file_v2_control(
                    handle,
                    channel,
                    topic,
                    BC_FILE_PKT_DONE,
                    expected_seq,
                    transfer_id);
            }
            return BC_OK;
        }

        fprintf(stderr, "boardcomm file_recv: ignore unknown packet type=%u topic=%s\n", hdr.type, topic);
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
