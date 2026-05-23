#include "transport_manager.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef int (*bc_transport_plugin_init_fn)(bc_transport_t *transport, const bc_transport_config_t *cfg);

static int load_plugin_transport(
    bc_transport_manager_t *manager,
    const char *plugin_path,
    const bc_transport_config_t *cfg)
{
    void *handle;
    bc_transport_plugin_init_fn init_fn;
    bc_transport_t *transport;
    char symbol[64];

    if (manager->count >= BC_MAX_TRANSPORTS) {
        return BC_ERR_NOMEM;
    }

    handle = dlopen(plugin_path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        return BC_ERR_NOT_FOUND;
    }

    snprintf(symbol, sizeof(symbol), "bc_%s_transport_init", cfg->type == BC_TRANSPORT_TCP ? "tcp" : "udp");
    init_fn = (bc_transport_plugin_init_fn)dlsym(handle, symbol);
    if (init_fn == NULL) {
        dlclose(handle);
        return BC_ERR_NOT_FOUND;
    }

    transport = &manager->transports[manager->count];
    if (init_fn(transport, cfg) != BC_OK) {
        dlclose(handle);
        return BC_ERR;
    }
    if (transport->ops == NULL || transport->ops->open == NULL ||
        transport->ops->open(transport) != BC_OK) {
        if (transport->ops != NULL && transport->ops->close != NULL) {
            transport->ops->close(transport);
        }
        dlclose(handle);
        return BC_ERR;
    }

    manager->count++;
    return BC_OK;
}

int bc_transport_manager_init(bc_transport_manager_t *manager, const bc_config_t *cfg)
{
    memset(manager, 0, sizeof(*manager));

    for (size_t i = 0; i < cfg->transport_count && i < BC_MAX_TRANSPORTS; ++i) {
        bc_transport_t *transport = &manager->transports[manager->count];
        const bc_transport_config_t *tcfg = &cfg->transports[i];
        int rc;

        switch (tcfg->type) {
        case BC_TRANSPORT_TCP:
            rc = bc_tcp_transport_init(transport, tcfg);
            break;
        case BC_TRANSPORT_UART:
            rc = bc_uart_transport_init(transport, tcfg);
            break;
        case BC_TRANSPORT_UDP_KCP:
            rc = bc_udp_kcp_transport_init(transport, tcfg);
            break;
        case BC_TRANSPORT_UDP:
        default:
            rc = bc_udp_transport_init(transport, tcfg);
            break;
        }

        if (rc != BC_OK) {
            continue;
        }
        if (transport->ops->open(transport) == BC_OK) {
            manager->count++;
        }
    }

    for (size_t i = 0; i < cfg->plugin_count; ++i) {
        if (cfg->transport_count == 0) {
            break;
        }
        (void)load_plugin_transport(manager, cfg->plugins[i], &cfg->transports[0]);
    }

    return manager->count > 0 ? BC_OK : BC_ERR_NOT_FOUND;
}

void bc_transport_manager_close(bc_transport_manager_t *manager)
{
    for (size_t i = 0; i < manager->count; ++i) {
        if (manager->transports[i].ops != NULL && manager->transports[i].ops->close != NULL) {
            manager->transports[i].ops->close(&manager->transports[i]);
        } else if (manager->transports[i].fd >= 0) {
            close(manager->transports[i].fd);
        }
    }
}

bc_transport_t *bc_transport_manager_find(bc_transport_manager_t *manager, const char *name)
{
    for (size_t i = 0; i < manager->count; ++i) {
        if (strcmp(manager->transports[i].name, name) == 0) {
            return &manager->transports[i];
        }
    }
    return NULL;
}
