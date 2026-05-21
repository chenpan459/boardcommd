#include "stats.h"

#include <string.h>

void bc_stats_reset(bc_stats_t *stats)
{
    if (stats != NULL) {
        memset(stats, 0, sizeof(*stats));
    }
}
