#ifndef LOG_H
#define LOG_H

#define LOG_LEVEL_DEBUG 0
#define LOG_LEVEL_INFO  1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_ERROR 3

void log_init(void);
void log_write(const char* text);
void log_printf(const char* format, ...);
void log_set_level(int level);
int log_get_level(void);
int log_get_count(void);
const char* log_get_line(int index);

#ifndef klog_info
#define klog_info(fmt, ...)  log_printf(fmt "\n", ##__VA_ARGS__)
#endif
#ifndef klog_warn
#define klog_warn(fmt, ...)  log_printf(fmt "\n", ##__VA_ARGS__)
#endif
#ifndef klog_error
#define klog_error(fmt, ...) log_printf(fmt "\n", ##__VA_ARGS__)
#endif

#endif
