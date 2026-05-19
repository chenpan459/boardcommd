#ifndef BC_TRANSPORT_MANAGER_H
#define BC_TRANSPORT_MANAGER_H

#include "config.h"
#include "transport.h"

typedef struct {
    bc_transport_t transports[BC_MAX_TRANSPORTS];
    size_t count;
} bc_transport_manager_t;

int bc_transport_manager_init(bc_transport_manager_t *manager, const bc_config_t *cfg);
void bc_transport_manager_close(bc_transport_manager_t *manager);
bc_transport_t *bc_transport_manager_find(bc_transport_manager_t *manager, const char *name);

#endif
