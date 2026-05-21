#include "topic_match.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    assert(bc_topic_matches("*", "telemetry.temp") == 1);
    assert(bc_topic_matches("telemetry.*", "telemetry.temp") == 1);
    assert(bc_topic_matches("telemetry.*", "control.run") == 0);
    assert(bc_topic_matches("control.run", "control.run") == 1);
    assert(bc_topic_matches("control.run", "control.stop") == 0);

    printf("test_topic_match: ok\n");
    return 0;
}
