#include "bc.h"
#include "bc_log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    unsigned int duration_sec;
    uint64_t received;
    uint64_t lost;
    uint64_t last_seq;
    int have_seq;
    size_t bytes;
    double start;
    double last_report;
    uint64_t last_received;
    uint64_t last_lost;
    size_t last_bytes;
} perf_state_t;

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void report_interval(perf_state_t *state, double now)
{
    double interval = now - state->last_report;
    uint64_t interval_received = state->received - state->last_received;
    uint64_t interval_lost = state->lost - state->last_lost;
    size_t interval_bytes = state->bytes - state->last_bytes;

    if (interval <= 0.0) {
        interval = 0.000001;
    }

    BC_LOGI(
        "perf_sub",
        "interval=%.3f-%.3f sec received=%llu lost=%llu rate=%.2f msg/s bitrate=%.2f Mbps",
        state->last_report - state->start,
        now - state->start,
        (unsigned long long)interval_received,
        (unsigned long long)interval_lost,
        (double)interval_received / interval,
        ((double)interval_bytes * 8.0) / interval / 1000000.0);

    state->last_report = now;
    state->last_received = state->received;
    state->last_lost = state->lost;
    state->last_bytes = state->bytes;
}

static void report_summary(perf_state_t *state)
{
    double elapsed = now_sec() - state->start;

    if (elapsed <= 0.0) {
        elapsed = 0.000001;
    }

    BC_LOGI(
        "perf_sub",
        "summary duration=%.3fs received=%llu lost=%llu bytes=%zu rate=%.2f msg/s bitrate=%.2f Mbps",
        elapsed,
        (unsigned long long)state->received,
        (unsigned long long)state->lost,
        state->bytes,
        (double)state->received / elapsed,
        ((double)state->bytes * 8.0) / elapsed / 1000000.0);
}

static void on_message(const char *topic, const void *payload, size_t len, perf_state_t *state)
{
    double now = now_sec();

    (void)topic;
    if (len >= sizeof(uint64_t)) {
        uint64_t seq;

        memcpy(&seq, payload, sizeof(seq));
        if (state->have_seq && seq > state->last_seq + 1) {
            state->lost += seq - state->last_seq - 1;
        }
        state->last_seq = seq;
        state->have_seq = 1;
    }

    state->received++;
    state->bytes += len;

    if (now - state->last_report >= 1.0) {
        report_interval(state, now);
    }
}

static void usage(const char *prog)
{
    BC_LOGI("perf_sub", "usage: %s [topic] [duration_sec]", prog);
    BC_LOGI("perf_sub", "example: %s perf.topic 10", prog);
}

int main(int argc, char **argv)
{
    const char *topic = argc > 1 ? argv[1] : "perf.topic";
    perf_state_t state;
    int handle;

    memset(&state, 0, sizeof(state));
    state.duration_sec = argc > 2 ? (unsigned int)strtoul(argv[2], NULL, 10) : 10;
    state.start = now_sec();
    state.last_report = state.start;

    (void)bc_log_init("log", "boardcomm_perf_sub");

    if (state.duration_sec == 0) {
        usage(argv[0]);
        BC_LOGE("perf_sub", "duration_sec must be > 0");
        bc_log_close();
        return 1;
    }

    handle = bc_open(NULL);
    if (handle < 0) {
        BC_LOGE("perf_sub", "failed to connect to boardcommd");
        bc_log_close();
        return 1;
    }

    if (bc_subscribe_fd(handle, topic) != BC_OK) {
        BC_LOGE("perf_sub", "failed to subscribe topic=%s", topic);
        (void)bc_close(handle);
        bc_log_close();
        return 1;
    }

    BC_LOGI(
        "perf_sub",
        "start topic=%s duration=%us",
        topic,
        state.duration_sec);

    while (now_sec() - state.start < (double)state.duration_sec) {
        char recv_topic[BC_MAX_TOPIC_LEN];
        uint8_t payload[BC_MAX_PAYLOAD_LEN];
        ssize_t n = bc_read(
            handle,
            recv_topic,
            sizeof(recv_topic),
            payload,
            sizeof(payload),
            100);

        if (n == BC_ERR_NOT_FOUND) {
            double now = now_sec();

            if (now - state.last_report >= 1.0) {
                report_interval(&state, now);
            }
            continue;
        }
        if (n < 0) {
            BC_LOGE("perf_sub", "read failed");
            (void)bc_close(handle);
            bc_log_close();
            return 1;
        }
        on_message(recv_topic, payload, (size_t)n, &state);
    }

    report_summary(&state);
    (void)bc_close(handle);
    bc_log_close();
    return 0;
}
