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

static void parse_tcp_role(bc_transport_config_t *transport, const char *role)
{
    transport->tcp_listen = BC_TCP_ROLE_AUTO;
    if (role == NULL || role[0] == '\0') {
        return;
    }
    if (strcmp(role, "server") == 0 || strcmp(role, "listen") == 0) {
        transport->tcp_listen = 1;
    } else if (strcmp(role, "client") == 0 || strcmp(role, "connect") == 0) {
        transport->tcp_listen = 0;
    }
}

void bc_config_load_defaults(bc_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->node_id = 1;
    cfg->bridge_broadcast = 1;
    snprintf(cfg->socket_path, sizeof(cfg->socket_path), "%s", BC_DEFAULT_SOCKET_PATH);

    bc_transport_config_t *udp = &cfg->transports[cfg->transport_count++];
    snprintf(udp->name, sizeof(udp->name), "udp0");
    udp->type = BC_TRANSPORT_UDP;
    snprintf(udp->endpoint, sizeof(udp->endpoint), "127.0.0.1:9101");
    udp->local_port = 9100;

    bc_route_config_t *route = &cfg->routes[cfg->route_count++];
    snprintf(route->topic, sizeof(route->topic), "*");
    snprintf(route->transport, sizeof(route->transport), "udp0");

    bc_channel_config_t *channel = &cfg->channels[cfg->channel_count++];
    snprintf(channel->name, sizeof(channel->name), "default");
    snprintf(channel->transport, sizeof(channel->transport), "udp0");
}

static int transport_exists(const bc_config_t *cfg, const char *name)
{
    for (size_t i = 0; i < cfg->transport_count; ++i) {
        if (strcmp(cfg->transports[i].name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

int bc_config_validate(const bc_config_t *cfg)
{
    if (cfg == NULL || cfg->node_id == 0) {
        return BC_ERR_INVALID;
    }
    if (cfg->transport_count == 0) {
        return BC_ERR_NOT_FOUND;
    }

    for (size_t i = 0; i < cfg->transport_count; ++i) {
        for (size_t j = i + 1; j < cfg->transport_count; ++j) {
            if (strcmp(cfg->transports[i].name, cfg->transports[j].name) == 0) {
                return BC_ERR_INVALID;
            }
        }
    }

    for (size_t i = 0; i < cfg->channel_count; ++i) {
        if (!transport_exists(cfg, cfg->channels[i].transport)) {
            return BC_ERR_NOT_FOUND;
        }
    }

    for (size_t i = 0; i < cfg->route_count; ++i) {
        if (!transport_exists(cfg, cfg->routes[i].transport)) {
            return BC_ERR_NOT_FOUND;
        }
    }

    return BC_OK;
}

int bc_config_load(const char *path, bc_config_t *cfg)
{
    char line[256];
    FILE *fp;

    bc_config_load_defaults(cfg);
    if (path == NULL) {
        return bc_config_validate(cfg);
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        return BC_ERR_NOT_FOUND;
    }

    cfg->transport_count = 0;
    cfg->channel_count = 0;
    cfg->route_count = 0;
    cfg->plugin_count = 0;

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
        if (sscanf(line, "socket_uid %u", &cfg->socket_uid) == 1) {
            continue;
        }
        if (strncmp(line, "require_route", 13) == 0) {
            cfg->require_route = 1;
            continue;
        }
        if (strncmp(line, "no_bridge", 9) == 0) {
            cfg->bridge_broadcast = 0;
            continue;
        }
        if (sscanf(line, "%31s", kind) != 1) {
            continue;
        }
        if (strcmp(kind, "plugin") == 0 && cfg->plugin_count < BC_MAX_TRANSPORTS) {
            char plugin_path[128] = {0};

            if (sscanf(line, "plugin %127s", plugin_path) == 1) {
                snprintf(
                    cfg->plugins[cfg->plugin_count],
                    sizeof(cfg->plugins[cfg->plugin_count]),
                    "%s",
                    plugin_path);
                cfg->plugin_count++;
            }
            continue;
        }
        if (strcmp(kind, "transport") == 0 && cfg->transport_count < BC_MAX_TRANSPORTS) {
            char name[32] = {0};
            char type[16] = {0};
            char endpoint[128] = {0};
            char role[16] = {0};
            int local_port = 0;
            int baudrate = 0;
            int fields;

            fields = sscanf(line, "transport %31s %15s %127s %d %d %15s",
                            name, type, endpoint, &local_port, &baudrate, role);
            if (fields >= 4) {
                bc_transport_config_t *transport = &cfg->transports[cfg->transport_count++];

                snprintf(transport->name, sizeof(transport->name), "%s", name);
                transport->type = parse_transport_type(type);
                snprintf(transport->endpoint, sizeof(transport->endpoint), "%s", endpoint);
                transport->local_port = local_port;
                transport->baudrate = baudrate;
                transport->tcp_listen = BC_TCP_ROLE_AUTO;
                if (transport->type == BC_TRANSPORT_TCP && fields >= 6) {
                    parse_tcp_role(transport, role);
                }
            }
            continue;
        }
        if (strcmp(kind, "channel") == 0 && cfg->channel_count < BC_MAX_CHANNELS) {
            char name[BC_MAX_CHANNEL_LEN] = {0};
            char transport[32] = {0};

            if (sscanf(line, "channel %31s %31s", name, transport) == 2) {
                bc_channel_config_t *channel = &cfg->channels[cfg->channel_count++];
                snprintf(channel->name, sizeof(channel->name), "%s", name);
                snprintf(channel->transport, sizeof(channel->transport), "%s", transport);
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
    return bc_config_validate(cfg);
}
