#ifndef BC_CONFIG_H
#define BC_CONFIG_H

#include <stddef.h>

#include "bc_types.h"

#define BC_MAX_TRANSPORTS 8
#define BC_MAX_ROUTES 32
#define BC_MAX_CHANNELS 32

typedef enum {
    BC_TRANSPORT_UDP,
    BC_TRANSPORT_UDP_KCP,
    BC_TRANSPORT_TCP,
    BC_TRANSPORT_UART,
} bc_transport_type_t;

/* TCP only: -1 = auto (local_port>0 => listen), 0 = client, 1 = server */
#define BC_TCP_ROLE_AUTO (-1)

/* UART: RS-232 full duplex vs RS-485 half duplex (DE GPIO) */
#define BC_UART_MODE_232 0
#define BC_UART_MODE_485 1
#define BC_UART_GPIO_NONE (-1)

typedef struct {
    char name[32];
    bc_transport_type_t type;
    char endpoint[128];
    int local_port;
    int baudrate;
    int tcp_listen;
    int kcp_conv;
    int uart_mode;
    int rs485_de_gpio;
} bc_transport_config_t;

typedef struct {
    char topic[BC_MAX_TOPIC_LEN];
    char transport[32];
} bc_route_config_t;

typedef struct {
    char name[BC_MAX_CHANNEL_LEN];
    char transport[32];
} bc_channel_config_t;

typedef struct {
    unsigned int node_id;
    char socket_path[128];
    unsigned int socket_uid;
    int require_route;
    int bridge_broadcast;
    char plugins[BC_MAX_TRANSPORTS][128];
    size_t plugin_count;
    bc_transport_config_t transports[BC_MAX_TRANSPORTS];
    size_t transport_count;
    bc_channel_config_t channels[BC_MAX_CHANNELS];
    size_t channel_count;
    bc_route_config_t routes[BC_MAX_ROUTES];
    size_t route_count;
} bc_config_t;

int bc_config_load(const char *path, bc_config_t *cfg);
void bc_config_load_defaults(bc_config_t *cfg);
int bc_config_validate(const bc_config_t *cfg);

#endif
