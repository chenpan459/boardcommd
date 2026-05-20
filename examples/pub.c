#include "boardcomm.h"
#include "log.h"

#include <string.h>

int main(int argc, char **argv)
{
    const char *topic = argc > 1 ? argv[1] : "demo.topic";
    const char *payload = argc > 2 ? argv[2] : "hello from boardcomm_pub";
    const char *channel = argc > 3 ? argv[3] : NULL;
    int handle;

    (void)bc_log_init("log", "boardcomm_pub");

    handle = bc_open(NULL);
    if (handle < 0) {
        BC_LOGE("pub", "failed to connect to boardcommd");
        bc_log_close();
        return 1;
    }

    if ((channel != NULL
             ? bc_write_channel(handle, channel, topic, payload, strlen(payload))
             : bc_write(handle, topic, payload, strlen(payload))) < 0) {
        BC_LOGE("pub", "failed to publish message");
        (void)bc_close(handle);
        bc_log_close();
        return 1;
    }

    BC_LOGI(
        "pub",
        "published channel=%s topic=%s payload=%s",
        channel != NULL ? channel : "-",
        topic,
        payload);
    (void)bc_close(handle);
    bc_log_close();
    return 0;
}
