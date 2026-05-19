#include "boardcomm.h"
#include "log.h"

#include <string.h>

int main(int argc, char **argv)
{
    const char *topic = argc > 1 ? argv[1] : "demo.topic";
    const char *payload = argc > 2 ? argv[2] : "hello from boardcomm_pub";

    (void)bc_log_init("log", "boardcomm_pub");

    if (boardcomm_init(NULL) != BC_OK) {
        BC_LOGE("pub", "failed to connect to boardcommd");
        bc_log_close();
        return 1;
    }

    if (boardcomm_publish(topic, payload, strlen(payload)) != BC_OK) {
        BC_LOGE("pub", "failed to publish message");
        boardcomm_shutdown();
        bc_log_close();
        return 1;
    }

    BC_LOGI("pub", "published topic=%s payload=%s", topic, payload);
    boardcomm_shutdown();
    bc_log_close();
    return 0;
}
