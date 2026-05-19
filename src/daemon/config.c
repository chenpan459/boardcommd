#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bc_transport_type_t parse_transport_type(const char *value)
{
    if (strcmp(value, "tcp") == 0) {
        return BC_TRANSPORT_TCP;
    }
    if (strcmp(value, "uart") == 0) {
        return BC_TRANSPORT_UART;
    }
    return BC_TRANSPORT_UDP;
}

void bc_config_load_defaults(bc_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->node_id = 1;
    snprintf(cfg->socket_path, sizeof(cfg->socket_path), "%s", BC_DEFAULT_SOCKET_PATH);

    bc_transport_config_t *udp = &cfg->transports[cfg->transport_count++];
    snprintf(udp->name, sizeof(udp->name), "udp0");
    udp->type = BC_TRANSPORT_UDP;
    snprintf(udp->endpoint, sizeof(udp->endpoint), "127.0.0.1:9101");
    udp->local_port = 9100;

    bc_route_config_t *route = &cfg->routes[cfg->route_count++];
    snprintf(route->topic, sizeof(route->topic), "*");
    snprintf(route->transport, sizeof(route->transport), "udp0");
}

int bc_config_load(const char *path, bc_config_t *cfg)
{
    char line[256];
    FILE *fp;

    bc_config_load_defaults(cfg);
    if (path == NULL) {
        return BC_OK;
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        return BC_OK;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char kind[32] = {0};

        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        if (sscanf(line, "node_id %u", &cfg->node_id) == 1) {
            continue;
        }
        if (sscanf(line, "socket %127s", cfg->socket_path) == 1) {
            continue;
        }
        if (sscanf(line, "%31s", kind) != 1) {
            continue;
        }
        if (strcmp(kind, "transport") == 0 && cfg->transport_count < BC_MAX_TRANSPORTS) {
            char name[32] = {0};
            char type[16] = {0};
            char endpoint[128] = {0};
            int local_port = 0;
            int baudrate = 0;

            if (sscanf(line, "transport %31s %15s %127s %d %d",
                       name, type, endpoint, &local_port, &baudrate) >= 4) {
                bc_transport_config_t *transport = &cfg->transports[cfg->transport_count++];
                snprintf(transport->name, sizeof(transport->name), "%s", name);
                transport->type = parse_transport_type(type);
                snprintf(transport->endpoint, sizeof(transport->endpoint), "%s", endpoint);
                transport->local_port = local_port;
                transport->baudrate = baudrate;
            }
            continue;
        }
        if (strcmp(kind, "route") == 0 && cfg->route_count < BC_MAX_ROUTES) {
            char topic[BC_MAX_TOPIC_LEN] = {0};
            char transport[32] = {0};

            if (sscanf(line, "route %63s %31s", topic, transport) == 2) {
                bc_route_config_t *route = &cfg->routes[cfg->route_count++];
                snprintf(route->topic, sizeof(route->topic), "%s", topic);
                snprintf(route->transport, sizeof(route->transport), "%s", transport);
            }
        }
    }

    fclose(fp);
    return BC_OK;
}
