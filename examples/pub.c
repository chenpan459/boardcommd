#include "boardcomm.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *topic = argc > 1 ? argv[1] : "demo.topic";
    const char *payload = argc > 2 ? argv[2] : "hello from boardcomm_pub";

    if (boardcomm_init(NULL) != BC_OK) {
        fprintf(stderr, "failed to connect to boardcommd\n");
        return 1;
    }

    if (boardcomm_publish(topic, payload, strlen(payload)) != BC_OK) {
        fprintf(stderr, "failed to publish message\n");
        boardcomm_shutdown();
        return 1;
    }

    printf("published topic=%s payload=%s\n", topic, payload);
    boardcomm_shutdown();
    return 0;
}
