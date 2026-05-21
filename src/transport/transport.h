#ifndef BC_TRANSPORT_H
#define BC_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

typedef struct bc_transport bc_transport_t;

typedef struct {
    int (*open)(bc_transport_t *transport);
    void (*close)(bc_transport_t *transport);
    int (*send)(bc_transport_t *transport, const uint8_t *data, size_t len);
    int (*get_fds)(bc_transport_t *transport, int *fds, size_t *count);
    int (*handle_event)(bc_transport_t *transport, int fd, uint32_t events);
} bc_transport_ops_t;

struct bc_transport {
    char name[32];
    bc_transport_type_t type;
    int fd;
    bc_transport_config_t config;
    const bc_transport_ops_t *ops;
    void *impl;
};

int bc_udp_transport_init(bc_transport_t *transport, const bc_transport_config_t *cfg);
int bc_tcp_transport_init(bc_transport_t *transport, const bc_transport_config_t *cfg);
int bc_uart_transport_init(bc_transport_t *transport, const bc_transport_config_t *cfg);

#endif
