#ifndef BC_LOG_H
#define BC_LOG_H

typedef enum {
    BC_LOG_DEBUG = 0,
    BC_LOG_INFO,
    BC_LOG_WARN,
    BC_LOG_ERROR,
} bc_log_level_t;

int bc_log_init(const char *log_dir, const char *name);
void bc_log_close(void);
void bc_log_write(bc_log_level_t level, const char *tag, const char *fmt, ...);

#define BC_LOGD(tag, ...) bc_log_write(BC_LOG_DEBUG, tag, __VA_ARGS__)
#define BC_LOGI(tag, ...) bc_log_write(BC_LOG_INFO, tag, __VA_ARGS__)
#define BC_LOGW(tag, ...) bc_log_write(BC_LOG_WARN, tag, __VA_ARGS__)
#define BC_LOGE(tag, ...) bc_log_write(BC_LOG_ERROR, tag, __VA_ARGS__)

#endif
