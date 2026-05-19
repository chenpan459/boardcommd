#include "boardcomm.h"
#include "log.h"

static void on_message(const char *topic, const void *payload, size_t len, void *user)
{
    (void)user;
    BC_LOGI("sub", "received topic=%s payload=%.*s", topic, (int)len, (const char *)payload);
}

int main(int argc, char **argv)
{
    const char *topic = argc > 1 ? argv[1] : "demo.topic";

    (void)bc_log_init("log", "boardcomm_sub");

    if (boardcomm_init(NULL) != BC_OK) {
        BC_LOGE("sub", "failed to connect to boardcommd");
        bc_log_close();
        return 1;
    }

    if (boardcomm_subscribe(topic, on_message, NULL) != BC_OK) {
        BC_LOGE("sub", "failed to subscribe topic=%s", topic);
        boardcomm_shutdown();
        bc_log_close();
        return 1;
    }

    BC_LOGI("sub", "subscribed topic=%s", topic);
    while (boardcomm_poll(-1) == BC_OK) {
    }

    boardcomm_shutdown();
    bc_log_close();
    return 0;
}
