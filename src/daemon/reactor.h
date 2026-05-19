#ifndef BC_REACTOR_H
#define BC_REACTOR_H

#include <stdint.h>

typedef enum {
    BC_REACTOR_SERVER,
    BC_REACTOR_CLIENT,
    BC_REACTOR_TRANSPORT,
} bc_reactor_fd_type_t;

typedef void (*bc_reactor_cb)(int fd, uint32_t events, void *user);

typedef struct bc_reactor_event bc_reactor_event_t;

typedef struct {
    int epoll_fd;
    int stop;
    bc_reactor_event_t *events;
} bc_reactor_t;

int bc_reactor_init(bc_reactor_t *reactor);
void bc_reactor_close(bc_reactor_t *reactor);
int bc_reactor_add(bc_reactor_t *reactor, int fd, uint32_t events, bc_reactor_cb cb, void *user);
int bc_reactor_mod(bc_reactor_t *reactor, int fd, uint32_t events, bc_reactor_cb cb, void *user);
void bc_reactor_del(bc_reactor_t *reactor, int fd);
int bc_reactor_run(bc_reactor_t *reactor);
void bc_reactor_stop(bc_reactor_t *reactor);

#endif
