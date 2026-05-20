#include "router.h"

#include <string.h>

void bc_router_init(
    bc_router_t *router,
    const bc_config_t *config,
    bc_transport_t *transports,
    size_t transport_count)
{
    router->config = config;
    router->transports = transports;
    router->transport_count = transport_count;
}

static bc_transport_t *find_transport(bc_router_t *router, const char *name)
{
    for (size_t i = 0; i < router->transport_count; ++i) {
        if (strcmp(router->transports[i].name, name) == 0) {
            return &router->transports[i];
        }
    }
    return NULL;
}

static int topic_matches(const char *pattern, const char *topic)
{
    size_t len;

    if (strcmp(pattern, "*") == 0) {
        return 1;
    }

    len = strlen(pattern);
    if (len > 0 && pattern[len - 1] == '*') {
        return strncmp(pattern, topic, len - 1) == 0;
    }

    return strcmp(pattern, topic) == 0;
}

int bc_router_route(
    bc_router_t *router,
    const bc_message_t *msg,
    bc_route_result_t *result)
{
    bc_transport_t *fallback = NULL;

    if (router == NULL || msg == NULL || result == NULL) {
        return BC_ERR_INVALID;
    }

    memset(result, 0, sizeof(*result));
    if (msg->channel[0] != '\0') {
        for (size_t i = 0; i < router->config->channel_count; ++i) {
            const bc_channel_config_t *channel = &router->config->channels[i];

            if (strcmp(channel->name, msg->channel) == 0) {
                result->transport = find_transport(router, channel->transport);
                return result->transport != NULL ? BC_OK : BC_ERR_NOT_FOUND;
            }
        }
        return BC_ERR_NOT_FOUND;
    }

    for (size_t i = 0; i < router->config->route_count; ++i) {
        const bc_route_config_t *route = &router->config->routes[i];

        if (strcmp(route->topic, "*") == 0) {
            fallback = find_transport(router, route->transport);
            continue;
        }
        if (topic_matches(route->topic, msg->topic)) {
            result->transport = find_transport(router, route->transport);
            return result->transport != NULL ? BC_OK : BC_ERR_NOT_FOUND;
        }
    }

    result->transport = fallback;
    return result->transport != NULL ? BC_OK : BC_ERR_NOT_FOUND;
}
