#ifndef BOARDCOMM_H
#define BOARDCOMM_H

#include "boardcomm_types.h"

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int bc_open(const char *socket_path);
int bc_close(int handle);

ssize_t bc_write(
    int handle,
    const char *topic,
    const void *payload,
    size_t len);

ssize_t bc_write_channel(
    int handle,
    const char *channel,
    const char *topic,
    const void *payload,
    size_t len);

ssize_t bc_read(
    int handle,
    char *topic,
    size_t topic_cap,
    void *payload,
    size_t payload_cap,
    int timeout_ms);

int bc_subscribe_fd(int handle, const char *topic);

int bc_init(const char *socket_path);
void bc_shutdown(void);

int bc_publish(
    const char *topic,
    const void *payload,
    size_t len);

int bc_subscribe(
    const char *topic,
    bc_msg_cb cb,
    void *user);

int bc_poll(int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
