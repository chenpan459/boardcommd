#ifndef BC_CONFIG_H
#define BC_CONFIG_H

#include <stddef.h>

#include "bc_types.h"

#define BC_MAX_TRANSPORTS 8
#define BC_MAX_ROUTES 32
#define BC_MAX_CHANNELS 32

typedef enum {
    BC_TRANSPORT_UDP,
    BC_TRANSPORT_TCP,
    BC_TRANSPORT_UART,
} bc_transport_type_t;

typedef struct {
    char name[32];
    bc_transport_type_t type;
    char endpoint[128];
    int local_port;
    int baudrate;
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
    bc_transport_config_t transports[BC_MAX_TRANSPORTS];
    size_t transport_count;
    bc_channel_config_t channels[BC_MAX_CHANNELS];
    size_t channel_count;
    bc_route_config_t routes[BC_MAX_ROUTES];
    size_t route_count;
} bc_config_t;

int bc_config_load(const char *path, bc_config_t *cfg);
void bc_config_load_defaults(bc_config_t *cfg);

#endif
