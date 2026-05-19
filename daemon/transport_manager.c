#include "transport_manager.h"

#include <string.h>
#include <unistd.h>

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
