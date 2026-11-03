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

static bool g_serial_present = true;

static void serial_init(void) {
    outb(0x3F8 + 1, 0x00); // Disable all interrupts
    outb(0x3F8 + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(0x3F8 + 0, 0x01); // Set divisor to 1 (lo byte) 115200 baud
    outb(0x3F8 + 1, 0x00); //                  (hi byte)
    outb(0x3F8 + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(0x3F8 + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
    outb(0x3F8 + 4, 0x03); // RTS/DSR set
    if (inb(0x3F8 + 5) == 0xFF) {
        g_serial_present = false;
    }
}

static void serial_putc(char c) {
    if (!g_serial_present) return;
    int timeout = 4096;
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

static int append_string_len(char* dst, int pos, int max, const char* text, int max_chars) {
    if (!text || (uintptr_t)text < 0x1000) {
        text = "(null)";
    }
    int count = 0;
    while (*text && (max_chars < 0 || count < max_chars)) {
        pos = append_char(dst, pos, max, *text++);
        count++;
    }
    return pos;
}

static int append_string(char* dst, int pos, int max, const char* text) {
    return append_string_len(dst, pos, max, text, -1);
}

static int append_unsigned64(char* dst, int pos, int max, uint64_t value,
                             unsigned int base, int uppercase, int min_width, char pad_char) {
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char temporary[64];
    int count = 0;
    if (value == 0) temporary[count++] = '0';
    while (value && count < (int)sizeof(temporary)) {
        temporary[count++] = digits[value % base];
        value /= base;
    }
    while (count < min_width && count < (int)sizeof(temporary)) {
        temporary[count++] = pad_char;
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

        format++; // Skip '%'
        if (*format == '%') {
            pos = append_char(line, pos, LOG_LINE_SIZE, '%');
            format++;
            continue;
        }

        // Parse zero-padding flag
        char pad_char = ' ';
        if (*format == '0') {
            pad_char = '0';
            format++;
        }

        // Parse minimum field width
        int min_width = 0;
        while (*format >= '0' && *format <= '9') {
            min_width = min_width * 10 + (*format - '0');
            format++;
        }

        // Parse precision (e.g. %.6s)
        int precision = -1;
        if (*format == '.') {
            format++;
            precision = 0;
            while (*format >= '0' && *format <= '9') {
                precision = precision * 10 + (*format - '0');
                format++;
            }
        }

        // Parse length modifiers (l, ll, h, z)
        bool is_long = false;
        if (*format == 'l') {
            is_long = true;
            format++;
            if (*format == 'l') format++;
        } else if (*format == 'z') {
            is_long = true;
            format++;
        } else if (*format == 'h') {
            format++;
            if (*format == 'h') format++;
        }

        switch (*format) {
            case 's': {
                const char* s = va_arg(args, const char*);
                pos = append_string_len(line, pos, LOG_LINE_SIZE, s, precision);
                break;
            }
            case 'c': {
                char c = (char)va_arg(args, int);
                pos = append_char(line, pos, LOG_LINE_SIZE, c);
                break;
            }
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
                pos = append_unsigned64(line, pos, LOG_LINE_SIZE, magnitude, 10, 0, min_width, pad_char);
                break;
            }
            case 'u': {
                uint64_t value = is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
                pos = append_unsigned64(line, pos, LOG_LINE_SIZE, value, 10, 0, min_width, pad_char);
                break;
            }
            case 'x': {
                uint64_t value = is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
                pos = append_unsigned64(line, pos, LOG_LINE_SIZE, value, 16, 0, min_width, pad_char);
                break;
            }
            case 'X': {
                uint64_t value = is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
                pos = append_unsigned64(line, pos, LOG_LINE_SIZE, value, 16, 1, min_width, pad_char);
                break;
            }
            case 'p': {
                pos = append_string(line, pos, LOG_LINE_SIZE, "0x");
                uint64_t ptr_val = (uint64_t)va_arg(args, void*);
                pos = append_unsigned64(line, pos, LOG_LINE_SIZE, ptr_val, 16, 0, 16, '0');
                break;
            }
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
