#ifndef LOG_H
#define LOG_H

#include <stdint.h>

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

uint32_t log_ring_pending(void);
uint32_t log_ring_peek(uint8_t* dst, uint32_t max_bytes,
                       uint32_t* out_head, uint32_t* out_dropped);
void log_ring_consume(uint32_t expected_head, uint32_t count);

void log_disk_init(void);
void log_disk_poll(void);
void log_disk_flush(void);
void log_disk_shutdown(void);

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
