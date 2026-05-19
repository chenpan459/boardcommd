#ifndef BOARDCOMM_H
#define BOARDCOMM_H

#include "boardcomm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

int boardcomm_init(const char *socket_path);
void boardcomm_shutdown(void);

int boardcomm_publish(
    const char *topic,
    const void *payload,
    size_t len);

int boardcomm_subscribe(
    const char *topic,
    boardcomm_msg_cb cb,
    void *user);

int boardcomm_poll(int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
