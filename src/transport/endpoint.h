#ifndef BC_TRANSPORT_ENDPOINT_H
#define BC_TRANSPORT_ENDPOINT_H

#include "bc_types.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

static inline int bc_parse_endpoint(const char *endpoint, char *host, size_t host_len, int *port)
{
    const char *colon = strrchr(endpoint, ':');
    size_t len;

    if (colon == NULL) {
        return BC_ERR_INVALID;
    }

    len = (size_t)(colon - endpoint);
    if (len == 0 || len >= host_len) {
        return BC_ERR_INVALID;
    }

    memcpy(host, endpoint, len);
    host[len] = '\0';
    *port = atoi(colon + 1);
    return *port > 0 ? BC_OK : BC_ERR_INVALID;
}

static inline int bc_fill_sockaddr_in(const char *host, int port, struct sockaddr_in *addr)
{
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr->sin_addr) != 1) {
        return BC_ERR_INVALID;
    }
    return BC_OK;
}

#endif
