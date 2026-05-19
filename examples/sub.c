#include "boardcomm.h"

#include <stdio.h>

static void on_message(const char *topic, const void *payload, size_t len, void *user)
{
    (void)user;
    printf("received topic=%s payload=%.*s\n", topic, (int)len, (const char *)payload);
}

int main(int argc, char **argv)
{
    const char *topic = argc > 1 ? argv[1] : "demo.topic";

    if (boardcomm_init(NULL) != BC_OK) {
        fprintf(stderr, "failed to connect to boardcommd\n");
        return 1;
    }

    if (boardcomm_subscribe(topic, on_message, NULL) != BC_OK) {
        fprintf(stderr, "failed to subscribe topic=%s\n", topic);
        boardcomm_shutdown();
        return 1;
    }

    printf("subscribed topic=%s\n", topic);
    while (boardcomm_poll(-1) == BC_OK) {
    }

    boardcomm_shutdown();
    return 0;
}
