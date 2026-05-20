#include "boardcomm.h"
#include "log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void usage(const char *prog)
{
    BC_LOGI("perf_pub", "usage: %s [topic] [duration_sec] [payload_size]", prog);
    BC_LOGI("perf_pub", "example: %s perf.topic 10 1024", prog);
}

int main(int argc, char **argv)
{
    const char *topic = argc > 1 ? argv[1] : "perf.topic";
    unsigned int duration_sec = argc > 2 ? (unsigned int)strtoul(argv[2], NULL, 10) : 10;
    size_t payload_size = argc > 3 ? (size_t)strtoul(argv[3], NULL, 10) : 1024;
    uint8_t *payload;
    double start;
    double last_report;
    double elapsed;
    uint64_t sent = 0;
    uint64_t failed = 0;
    uint64_t last_sent = 0;
    uint64_t last_failed = 0;
    int handle;

    (void)bc_log_init("log", "boardcomm_perf_pub");

    if (duration_sec == 0 || payload_size < sizeof(uint64_t) || payload_size > BC_MAX_PAYLOAD_LEN) {
        usage(argv[0]);
        BC_LOGE("perf_pub", "duration_sec must be > 0 and payload_size must be between %zu and %d", sizeof(uint64_t), BC_MAX_PAYLOAD_LEN);
        bc_log_close();
        return 1;
    }

    payload = calloc(1, payload_size);
    if (payload == NULL) {
        BC_LOGE("perf_pub", "failed to allocate payload");
        bc_log_close();
        return 1;
    }
    memset(payload, 0x5a, payload_size);

    handle = bc_open(NULL);
    if (handle < 0) {
        BC_LOGE("perf_pub", "failed to connect to boardcommd");
        free(payload);
        bc_log_close();
        return 1;
    }

    BC_LOGI("perf_pub", "start topic=%s duration=%us payload_size=%zu", topic, duration_sec, payload_size);
    start = now_sec();
    last_report = start;
    while (now_sec() - start < (double)duration_sec) {
        double now;
        uint64_t seq = sent + failed;

        memcpy(payload, &seq, sizeof(seq));
        if (bc_write(handle, topic, payload, payload_size) >= 0) {
            sent++;
        } else {
            failed++;
        }

        now = now_sec();
        if (now - last_report >= 1.0) {
            double interval = now - last_report;
            uint64_t interval_sent = sent - last_sent;
            uint64_t interval_failed = failed - last_failed;

            BC_LOGI(
                "perf_pub",
                "interval=%.3f-%.3f sec sent=%llu failed=%llu rate=%.2f msg/s bitrate=%.2f Mbps",
                last_report - start,
                now - start,
                (unsigned long long)interval_sent,
                (unsigned long long)interval_failed,
                (double)interval_sent / interval,
                ((double)interval_sent * (double)payload_size * 8.0) / interval / 1000000.0);

            last_report = now;
            last_sent = sent;
            last_failed = failed;
        }
    }
    elapsed = now_sec() - start;

    if (elapsed <= 0.0) {
        elapsed = 0.000001;
    }

    BC_LOGI(
        "perf_pub",
        "summary duration=%.3fs sent=%llu failed=%llu rate=%.2f msg/s bitrate=%.2f Mbps",
        elapsed,
        (unsigned long long)sent,
        (unsigned long long)failed,
        (double)sent / elapsed,
        ((double)sent * (double)payload_size * 8.0) / elapsed / 1000000.0);

    (void)bc_close(handle);
    free(payload);
    bc_log_close();
    return failed == 0 ? 0 : 1;
}
