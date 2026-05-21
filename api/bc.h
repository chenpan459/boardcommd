#ifndef BC_H
#define BC_H

#include "bc_types.h"

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bc_context bc_context_t;

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

ssize_t bc_write_ex(
    int handle,
    const char *channel,
    const char *topic,
    const void *payload,
    size_t len,
    const bc_publish_opts_t *opts);

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

bc_context_t *bc_context_create(const char *socket_path);
void bc_context_destroy(bc_context_t *ctx);
int bc_context_publish(
    bc_context_t *ctx,
    const char *topic,
    const void *payload,
    size_t len);
int bc_context_subscribe(bc_context_t *ctx, const char *topic, bc_msg_cb cb, void *user);
int bc_context_poll(bc_context_t *ctx, int timeout_ms);

int bc_enable_shm(int handle);
int bc_context_enable_shm(bc_context_t *ctx);
int bc_get_stats(int handle, bc_client_stats_t *stats);

int bc_discover_nodes(int handle, uint32_t *nodes, size_t cap, size_t *count);
int bc_persist_enable(int handle, const char *queue_path);

int bc_file_send(int handle, const char *topic, const char *path);
int bc_file_send_channel(
    int handle,
    const char *channel,
    const char *topic,
    const char *path);

int bc_file_recv(
    int handle,
    const char *topic,
    const char *path,
    int timeout_ms);
int bc_file_recv_channel(
    int handle,
    const char *channel,
    const char *topic,
    const char *path,
    int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
