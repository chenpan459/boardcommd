#include "bc.h"
#include "bc_log.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    const char *topic = argc > 1 ? argv[1] : "file.topic";
    const char *file_path = argc > 2 ? argv[2] : NULL;
    const char *channel = argc > 3 ? argv[3] : NULL;
    int handle;
    int rc;

    if (file_path == NULL) {
        fprintf(
            stderr,
            "usage: %s [topic] <file_path> [channel]\n",
            argv[0]);
        return 1;
    }

    (void)bc_log_init("log", "boardcomm_file_send");

    handle = bc_open(NULL);
    if (handle < 0) {
        BC_LOGE("file_send", "failed to connect to boardcommd");
        bc_log_close();
        return 1;
    }

    BC_LOGI(
        "file_send",
        "sending topic=%s file=%s channel=%s",
        topic,
        file_path,
        channel != NULL ? channel : "-");

    rc = (channel != NULL
              ? bc_file_send_channel(handle, channel, topic, file_path)
              : bc_file_send(handle, topic, file_path));
    if (rc != BC_OK) {
        BC_LOGE("file_send", "send failed rc=%d file=%s", rc, file_path);
        (void)bc_close(handle);
        bc_log_close();
        return 1;
    }

    BC_LOGI("file_send", "sent topic=%s file=%s", topic, file_path);
    (void)bc_close(handle);
    bc_log_close();
    return 0;
}
