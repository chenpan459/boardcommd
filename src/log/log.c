#include "log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static FILE *g_log_file;

static const char *level_name(bc_log_level_t level)
{
    switch (level) {
    case BC_LOG_DEBUG:
        return "DEBUG";
    case BC_LOG_INFO:
        return "INFO";
    case BC_LOG_WARN:
        return "WARN";
    case BC_LOG_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

int bc_log_init(const char *log_dir, const char *name)
{
    char path[256];

    if (log_dir == NULL || name == NULL || log_dir[0] == '\0' || name[0] == '\0') {
        return -1;
    }

    if (mkdir(log_dir, 0755) < 0 && errno != EEXIST) {
        return -1;
    }

    snprintf(path, sizeof(path), "%s/%s.log", log_dir, name);
    g_log_file = fopen(path, "a");
    return g_log_file != NULL ? 0 : -1;
}

void bc_log_close(void)
{
    if (g_log_file != NULL) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

void bc_log_write(bc_log_level_t level, const char *tag, const char *fmt, ...)
{
    char time_buf[32];
    time_t now = time(NULL);
    struct tm tm_now;
    FILE *stream = level >= BC_LOG_ERROR ? stderr : stdout;
    va_list ap;

    if (localtime_r(&now, &tm_now) != NULL) {
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_now);
    } else {
        snprintf(time_buf, sizeof(time_buf), "unknown-time");
    }

    fprintf(stream, "[%s] [%s] [%s] ", time_buf, level_name(level), tag != NULL ? tag : "-");
    va_start(ap, fmt);
    vfprintf(stream, fmt, ap);
    va_end(ap);
    fputc('\n', stream);
    fflush(stream);

    if (g_log_file != NULL) {
        fprintf(g_log_file, "[%s] [%s] [%s] ", time_buf, level_name(level), tag != NULL ? tag : "-");
        va_start(ap, fmt);
        vfprintf(g_log_file, fmt, ap);
        va_end(ap);
        fputc('\n', g_log_file);
        fflush(g_log_file);
    }
}
