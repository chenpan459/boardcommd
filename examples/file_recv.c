#include "bc.h"
#include "bc_log.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *topic = argc > 1 ? argv[1] : "file.topic";
    const char *save_path = argc > 2 ? argv[2] : "/tmp/boardcomm_recv.bin";
    int timeout_ms = argc > 3 ? atoi(argv[3]) : 30000;
    const char *channel = argc > 4 ? argv[4] : NULL;
    int handle;
    int rc;

    (void)bc_log_init("log", "boardcomm_file_recv");

    handle = bc_open_with_shm(NULL);
    if (handle < 0) {
        BC_LOGE("file_recv", "failed to connect to boardcommd");
        bc_log_close();
        return 1;
    }

    BC_LOGI(
        "file_recv",
        "ready topic=%s save=%s timeout_ms=%d channel=%s (start file_send now)",
        topic,
        save_path,
        timeout_ms,
        channel != NULL ? channel : "-");

    rc = (channel != NULL
              ? bc_file_recv_channel(handle, channel, topic, save_path, timeout_ms)
              : bc_file_recv(handle, topic, save_path, timeout_ms));
    if (rc != BC_OK) {
        if (rc == BC_ERR_TIMEOUT) {
            BC_LOGE(
                "file_recv",
                "timeout: start sender while receiver is waiting (timeout_ms=%d)",
                timeout_ms);
        } else if (rc == BC_ERR_IO) {
            BC_LOGE(
                "file_recv",
                "transfer error (chunks lost or crc mismatch); retry with receiver running first");
        } else {
            BC_LOGE("file_recv", "receive failed rc=%d", rc);
        }
        (void)bc_close(handle);
        bc_log_close();
        return 1;
    }

    BC_LOGI("file_recv", "saved topic=%s path=%s", topic, save_path);
    (void)bc_close(handle);
    bc_log_close();
    return 0;
}
