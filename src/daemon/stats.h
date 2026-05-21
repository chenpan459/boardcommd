#ifndef BC_STATS_H
#define BC_STATS_H

#include <stdint.h>

typedef struct {
    uint64_t pub_local;
    uint64_t pub_network;
    uint64_t pub_failed;
    uint64_t rx_network;
    uint64_t rx_local;
    uint64_t rx_forward;
    uint64_t rx_drop;
    uint64_t ack_sent;
    uint64_t ack_recv;
    uint64_t frag_rx;
    uint64_t frag_tx;
} bc_stats_t;

void bc_stats_reset(bc_stats_t *stats);

#endif
