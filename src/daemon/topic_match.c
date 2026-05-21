#include "topic_match.h"

#include <string.h>

int bc_topic_matches(const char *pattern, const char *topic)
{
    size_t len;

    if (pattern == NULL || topic == NULL) {
        return 0;
    }
    if (strcmp(pattern, "*") == 0) {
        return 1;
    }

    len = strlen(pattern);
    if (len > 0 && pattern[len - 1] == '*') {
        return strncmp(pattern, topic, len - 1) == 0;
    }

    return strcmp(pattern, topic) == 0;
}
