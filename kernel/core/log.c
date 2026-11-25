#include "log.h"
#include "port.h"
#include "spinlock.h"
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

static spinlock_t g_log_lock = SPINLOCK_INIT;

#define LOG_LINE_SIZE      160
#define LOG_HISTORY_LINES  64
#define LOG_DISK_RING_SIZE 131072u
#define LOG_DISK_RING_MASK (LOG_DISK_RING_SIZE - 1u)

static int g_initialized = 0;
static int g_level = LOG_LEVEL_DEBUG;
static bool g_full_log_mode = false;
static char g_history[LOG_HISTORY_LINES][LOG_LINE_SIZE];
static int g_history_count = 0;
static int g_history_next = 0;

static uint8_t g_disk_ring[LOG_DISK_RING_SIZE];
static volatile uint32_t g_disk_head = 0;
static volatile uint32_t g_disk_tail = 0;
static volatile uint32_t g_disk_dropped = 0;

static bool g_serial_present = true;

static inline uint64_t log_irq_save(void) {
    uint64_t flags = 0;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void log_irq_restore(uint64_t flags) {
    if (flags & (1ULL << 9)) {
        __asm__ volatile("sti" ::: "memory");
    }
}

static void log_ring_push_byte(uint8_t ch) {
    if ((uint32_t)(g_disk_tail - g_disk_head) >= LOG_DISK_RING_SIZE) {
        g_disk_head++;
        g_disk_dropped++;
    }
    g_disk_ring[g_disk_tail & LOG_DISK_RING_MASK] = ch;
    g_disk_tail++;
}

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

void log_set_full_mode(bool full) {
    g_full_log_mode = full;
}

bool log_is_full_mode(void) {
    return g_full_log_mode;
}

static char log_ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool log_starts_with_ci(const char* text, const char* prefix) {
    if (!text || !prefix) return false;
    while (*prefix) {
        if (log_ascii_lower(*text++) != log_ascii_lower(*prefix++)) return false;
    }
    return true;
}

static bool log_contains_ci(const char* text, const char* needle) {
    if (!text || !needle || !*needle) return false;
    for (int i = 0; text[i]; i++) {
        int j = 0;
        while (needle[j] && text[i + j] &&
               log_ascii_lower(text[i + j]) == log_ascii_lower(needle[j])) {
            j++;
        }
        if (!needle[j]) return true;
    }
    return false;
}

static bool log_line_is_urgent(const char* line) {
    if (!line || !line[0]) return false;
    /* Never recurse on disk-logger messages or high-frequency HID move traffic */
    if (log_starts_with_ci(line, "[LOG  ]")) return false;
    if (log_starts_with_ci(line, "[USBHID] report") ||
        log_starts_with_ci(line, "[USBHID] MOUSE") ||
        log_starts_with_ci(line, "[USBHID] BUTTON") ||
        log_starts_with_ci(line, "[INPUT]")) {
        return false;
    }
    if (log_starts_with_ci(line, "[xHCI]") ||
        log_starts_with_ci(line, "[USB ]") ||
        log_starts_with_ci(line, "[USBHOT]") ||
        log_starts_with_ci(line, "[USBMS]") ||
        log_starts_with_ci(line, "[USBHID]") ||
        log_starts_with_ci(line, "[PANIC]") ||
        log_starts_with_ci(line, "[TRAP ]") ||
        log_starts_with_ci(line, "[FATAL]")) {
        return true;
    }
    return log_contains_ci(line, "panic") ||
           log_contains_ci(line, "fault") ||
           log_contains_ci(line, "failed") ||
           log_contains_ci(line, "error") ||
           log_contains_ci(line, "timeout") ||
           log_contains_ci(line, "breaker");
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
    uint64_t flags = spin_lock_irqsave(&g_log_lock);
    int length = 0;
    int slot = g_history_next;
    while (line[length] && length < LOG_LINE_SIZE - 1) {
        g_history[slot][length] = line[length];
        length++;
    }
    g_history[slot][length] = '\0';
    g_history_next = (g_history_next + 1) % LOG_HISTORY_LINES;
    if (g_history_count < LOG_HISTORY_LINES) g_history_count++;

    for (int i = 0; line[i]; i++) {
        if (line[i] == '\n') log_ring_push_byte('\r');
        log_ring_push_byte((uint8_t)line[i]);
    }

    serial_puts(line);
    spin_unlock_irqrestore(&g_log_lock, flags);

    if (log_line_is_urgent(line)) {
        log_disk_notify_urgent();
    }
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

uint32_t log_ring_pending(void) {
    uint64_t flags = log_irq_save();
    uint32_t pending = g_disk_tail - g_disk_head;
    if (pending > LOG_DISK_RING_SIZE) pending = LOG_DISK_RING_SIZE;
    log_irq_restore(flags);
    return pending;
}

uint32_t log_ring_peek(uint8_t* dst, uint32_t max_bytes,
                       uint32_t* out_head, uint32_t* out_dropped) {
    if (!dst || max_bytes == 0) return 0;
    uint64_t flags = log_irq_save();
    uint32_t avail = g_disk_tail - g_disk_head;
    if (avail > LOG_DISK_RING_SIZE) {
        g_disk_head = g_disk_tail - LOG_DISK_RING_SIZE;
        avail = LOG_DISK_RING_SIZE;
    }
    uint32_t count = avail < max_bytes ? avail : max_bytes;
    uint32_t head = g_disk_head;
    for (uint32_t i = 0; i < count; i++) {
        dst[i] = g_disk_ring[(head + i) & LOG_DISK_RING_MASK];
    }
    if (out_head) *out_head = head;
    if (out_dropped) *out_dropped = g_disk_dropped;
    log_irq_restore(flags);
    return count;
}

void log_ring_consume(uint32_t expected_head, uint32_t count) {
    uint64_t flags = log_irq_save();
    uint32_t target = expected_head + count;
    if ((int32_t)(g_disk_head - expected_head) >= 0 &&
        (int32_t)(target - g_disk_head) > 0) {
        if ((int32_t)(g_disk_tail - target) < 0) {
            target = g_disk_tail;
        }
        g_disk_head = target;
    }
    g_disk_dropped = 0;
    log_irq_restore(flags);
}
