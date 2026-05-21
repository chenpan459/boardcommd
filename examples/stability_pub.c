#include "bc.h"
#include "bc_log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void usage(const char *prog)
{
    BC_LOGI("stability_pub", "usage: %s [topic] [duration_sec] [payload_size] [interval_us]", prog);
    BC_LOGI("stability_pub", "example: %s stable.topic 3600 256 10000", prog);
}

int main(int argc, char **argv)
{
    const char *topic = argc > 1 ? argv[1] : "stable.topic";
    unsigned int duration_sec = argc > 2 ? (unsigned int)strtoul(argv[2], NULL, 10) : 3600;
    size_t payload_size = argc > 3 ? (size_t)strtoul(argv[3], NULL, 10) : 256;
    useconds_t interval_us = argc > 4 ? (useconds_t)strtoul(argv[4], NULL, 10) : 10000;
    uint8_t *payload;
    double start;
    double last_report;
    uint64_t sent = 0;
    uint64_t failed = 0;
    int handle;

    (void)bc_log_init("log", "boardcomm_stability_pub");

    if (duration_sec == 0 || payload_size < sizeof(uint64_t) || payload_size > BC_MAX_PAYLOAD_LEN) {
        usage(argv[0]);
        BC_LOGE("stability_pub", "invalid duration or payload_size");
        bc_log_close();
        return 1;
    }

    payload = calloc(1, payload_size);
    if (payload == NULL) {
        BC_LOGE("stability_pub", "failed to allocate payload");
        bc_log_close();
        return 1;
    }
    memset(payload, 0xa5, payload_size);

    handle = bc_open(NULL);
    if (handle < 0) {
        BC_LOGE("stability_pub", "failed to connect to boardcommd");
        free(payload);
        bc_log_close();
        return 1;
    }

    start = now_sec();
    last_report = start;
    BC_LOGI(
        "stability_pub",
        "start topic=%s duration=%us payload_size=%zu interval_us=%u",
        topic,
        duration_sec,
        payload_size,
        (unsigned int)interval_us);

    while (now_sec() - start < (double)duration_sec) {
        double now;

        memcpy(payload, &sent, sizeof(sent));
        if (bc_write(handle, topic, payload, payload_size) >= 0) {
            sent++;
        } else {
            failed++;
        }

        now = now_sec();
        if (now - last_report >= 10.0) {
            double elapsed = now - start;

            BC_LOGI(
                "stability_pub",
                "progress sent=%llu failed=%llu elapsed=%.0fs msg_rate=%.2f msg/s",
                (unsigned long long)sent,
                (unsigned long long)failed,
                elapsed,
                (double)sent / elapsed);
            last_report = now;
        }

        if (interval_us > 0) {
            usleep(interval_us);
        }
    }

    {
        double elapsed = now_sec() - start;

        if (elapsed <= 0.0) {
            elapsed = 0.000001;
        }
        BC_LOGI(
            "stability_pub",
            "done sent=%llu failed=%llu elapsed=%.3fs msg_rate=%.2f msg/s fail_rate=%.6f",
            (unsigned long long)sent,
            (unsigned long long)failed,
            elapsed,
            (double)sent / elapsed,
            sent + failed > 0 ? (double)failed / (double)(sent + failed) : 0.0);
    }

    (void)bc_close(handle);
    free(payload);
    bc_log_close();
    return failed == 0 ? 0 : 1;
}
