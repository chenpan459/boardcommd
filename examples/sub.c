#include "bc.h"
#include "bc_log.h"

int main(int argc, char **argv)
{
    const char *topic = argc > 1 ? argv[1] : "demo.topic";
    int handle;

    (void)bc_log_init("log", "boardcomm_sub");

    handle = bc_open(NULL);
    if (handle < 0) {
        BC_LOGE("sub", "failed to connect to boardcommd");
        bc_log_close();
        return 1;
    }

    if (bc_subscribe_fd(handle, topic) != BC_OK) {
        BC_LOGE("sub", "failed to subscribe topic=%s", topic);
        (void)bc_close(handle);
        bc_log_close();
        return 1;
    }

    BC_LOGI("sub", "subscribed topic=%s", topic);
    for (;;) {
        char recv_topic[BC_MAX_TOPIC_LEN];
        uint8_t payload[BC_MAX_PAYLOAD_LEN];
        ssize_t n = bc_read(
            handle,
            recv_topic,
            sizeof(recv_topic),
            payload,
            sizeof(payload),
            -1);

        if (n < 0) {
            break;
        }
        BC_LOGI("sub", "received topic=%s payload=%.*s", recv_topic, (int)n, (const char *)payload);
    }

    (void)bc_close(handle);
    bc_log_close();
    return 0;
}
