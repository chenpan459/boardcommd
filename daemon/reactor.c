#include "reactor.h"

#include "boardcomm_types.h"

#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

typedef struct {
    int fd;
    bc_reactor_cb cb;
    void *user;
} bc_reactor_event_t;

int bc_reactor_init(bc_reactor_t *reactor)
{
    reactor->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    reactor->stop = 0;
    return reactor->epoll_fd >= 0 ? BC_OK : BC_ERR_IO;
}

void bc_reactor_close(bc_reactor_t *reactor)
{
    if (reactor->epoll_fd >= 0) {
        close(reactor->epoll_fd);
        reactor->epoll_fd = -1;
    }
}

int bc_reactor_add(bc_reactor_t *reactor, int fd, uint32_t events, bc_reactor_cb cb, void *user)
{
    struct epoll_event ev;
    bc_reactor_event_t *item = calloc(1, sizeof(*item));

    if (item == NULL) {
        return BC_ERR_NOMEM;
    }

    item->fd = fd;
    item->cb = cb;
    item->user = user;
    ev.events = events;
    ev.data.ptr = item;

    if (epoll_ctl(reactor->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        free(item);
        return BC_ERR_IO;
    }
    return BC_OK;
}

int bc_reactor_mod(bc_reactor_t *reactor, int fd, uint32_t events, bc_reactor_cb cb, void *user)
{
    struct epoll_event ev;
    bc_reactor_event_t *item = calloc(1, sizeof(*item));

    if (item == NULL) {
        return BC_ERR_NOMEM;
    }

    item->fd = fd;
    item->cb = cb;
    item->user = user;
    ev.events = events;
    ev.data.ptr = item;

    if (epoll_ctl(reactor->epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
        free(item);
        return BC_ERR_IO;
    }
    return BC_OK;
}

void bc_reactor_del(bc_reactor_t *reactor, int fd)
{
    epoll_ctl(reactor->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
}

int bc_reactor_run(bc_reactor_t *reactor)
{
    struct epoll_event events[32];

    while (!reactor->stop) {
        int n = epoll_wait(reactor->epoll_fd, events, 32, -1);

        if (n < 0) {
            continue;
        }

        for (int i = 0; i < n; ++i) {
            bc_reactor_event_t *item = events[i].data.ptr;

            if (item != NULL && item->cb != NULL) {
                item->cb(item->fd, events[i].events, item->user);
            }
        }
    }

    return BC_OK;
}

void bc_reactor_stop(bc_reactor_t *reactor)
{
    reactor->stop = 1;
}
