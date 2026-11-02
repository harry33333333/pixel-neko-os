#include "log.h"
#include "port.h"
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

#define LOG_LINE_SIZE     160
#define LOG_HISTORY_LINES 64

static int g_initialized = 0;
static int g_level = LOG_LEVEL_DEBUG;
static char g_history[LOG_HISTORY_LINES][LOG_LINE_SIZE];
static int g_history_count = 0;
static int g_history_next = 0;

static void serial_init(void) {
    outb(0x3F8 + 1, 0x00); // Disable all interrupts
    outb(0x3F8 + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(0x3F8 + 0, 0x03); // Set divisor to 3 (lo byte) 38400 baud
    outb(0x3F8 + 1, 0x00); //                  (hi byte)
    outb(0x3F8 + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(0x3F8 + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
    outb(0x3F8 + 4, 0x03); // RTS/DSR set
}

static void serial_putc(char c) {
    int timeout = 100000;
    while (!(inb(0x3F8 + 5) & 0x20) && --timeout) {
    }
    outb(0x3F8, (unsigned char)c);
}

static void serial_puts(const char* text) {
    while (text && *text) {
        if (*text == '\n') serial_putc('\r');
        serial_putc(*text++);
    }
}

void log_init(void) {
    if (g_initialized) return;
    serial_init();
    g_history_count = 0;
    g_history_next = 0;
    g_initialized = 1;
}

void log_set_level(int level) {
    g_level = level;
}

int log_get_level(void) {
    return g_level;
}

static int append_char(char* dst, int pos, int max, char c) {
    if (pos < max - 1) dst[pos++] = c;
    return pos;
}

static int append_string(char* dst, int pos, int max, const char* text) {
    if (!text) text = "(null)";
    while (*text) pos = append_char(dst, pos, max, *text++);
    return pos;
}

static int append_unsigned64(char* dst, int pos, int max, uint64_t value,
                             unsigned int base, int uppercase) {
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char temporary[32];
    int count = 0;
    if (value == 0) temporary[count++] = '0';
    while (value && count < (int)sizeof(temporary)) {
        temporary[count++] = digits[value % base];
        value /= base;
    }
    while (count > 0) pos = append_char(dst, pos, max, temporary[--count]);
    return pos;
}

static void log_commit(const char* line) {
    int length = 0;
    int slot = g_history_next;
    while (line[length] && length < LOG_LINE_SIZE - 1) {
        g_history[slot][length] = line[length];
        length++;
    }
    g_history[slot][length] = '\0';
    g_history_next = (g_history_next + 1) % LOG_HISTORY_LINES;
    if (g_history_count < LOG_HISTORY_LINES) g_history_count++;
    serial_puts(line);
}

void log_printf(const char* format, ...) {
    char line[LOG_LINE_SIZE];
    int pos = 0;
    va_list args;

    if (!g_initialized) log_init();
    if (!format) return;

    va_start(args, format);
    while (*format && pos < LOG_LINE_SIZE - 1) {
        if (*format != '%') {
            pos = append_char(line, pos, LOG_LINE_SIZE, *format++);
            continue;
        }

        format++;
        bool is_long = false;
        if (*format == 'l') {
            is_long = true;
            format++;
            if (*format == 'l') format++;
        }

        switch (*format) {
            case 's':
                pos = append_string(line, pos, LOG_LINE_SIZE, va_arg(args, const char*));
                break;
            case 'c':
                pos = append_char(line, pos, LOG_LINE_SIZE, (char)va_arg(args, int));
                break;
            case 'd':
            case 'i': {
                int64_t value = is_long ? va_arg(args, int64_t) : va_arg(args, int);
                uint64_t magnitude;
                if (value < 0) {
                    pos = append_char(line, pos, LOG_LINE_SIZE, '-');
                    magnitude = (uint64_t)(-value);
                } else {
                    magnitude = (uint64_t)value;
                }
                pos = append_unsigned64(line, pos, LOG_LINE_SIZE, magnitude, 10, 0);
                break;
            }
            case 'u': {
                uint64_t value = is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
                pos = append_unsigned64(line, pos, LOG_LINE_SIZE, value, 10, 0);
                break;
            }
            case 'x': {
                uint64_t value = is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
                pos = append_unsigned64(line, pos, LOG_LINE_SIZE, value, 16, 0);
                break;
            }
            case 'X': {
                uint64_t value = is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
                pos = append_unsigned64(line, pos, LOG_LINE_SIZE, value, 16, 1);
                break;
            }
            case 'p': {
                pos = append_string(line, pos, LOG_LINE_SIZE, "0x");
                uint64_t ptr_val = (uint64_t)va_arg(args, void*);
                pos = append_unsigned64(line, pos, LOG_LINE_SIZE, ptr_val, 16, 0);
                break;
            }
            case '%':
                pos = append_char(line, pos, LOG_LINE_SIZE, '%');
                break;
            case '\0':
                format--;
                break;
            default:
                pos = append_char(line, pos, LOG_LINE_SIZE, '%');
                pos = append_char(line, pos, LOG_LINE_SIZE, *format);
                break;
        }
        format++;
    }
    va_end(args);
    line[pos] = '\0';
    log_commit(line);
}

void log_write(const char* text) {
    if (!text) return;
    log_printf("%s", text);
}

int log_get_count(void) {
    return g_history_count;
}

const char* log_get_line(int index) {
    if (index < 0 || index >= g_history_count) return "";
    int first = (g_history_next - g_history_count + LOG_HISTORY_LINES) % LOG_HISTORY_LINES;
    return g_history[(first + index) % LOG_HISTORY_LINES];
}
