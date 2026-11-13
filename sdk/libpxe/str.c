#include <pxe/str.h>

void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    uint8_t v = (uint8_t)c;
    for (size_t i = 0; i < n; i++) d[i] = v;
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* p = (const uint8_t*)a;
    const uint8_t* q = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++) {
        if (p[i] != q[i]) return (int)p[i] - (int)q[i];
    }
    return 0;
}

size_t strlen(const char* s) {
    if (!s) return 0;
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

char* strcpy(char* dst, const char* src) {
    char* out = dst;
    while (*src) *dst++ = *src++;
    *dst = '\0';
    return out;
}

char* strncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char* strcat(char* dst, const char* src) {
    char* p = dst;
    while (*p) p++;
    while (*src) *p++ = *src++;
    *p = '\0';
    return dst;
}

int strcmp(const char* a, const char* b) {
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == '\0') return 0;
    }
    return 0;
}

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

int strcasecmp(const char* a, const char* b) {
    while (*a && *b) {
        char ca = ascii_lower(*a++);
        char cb = ascii_lower(*b++);
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
    }
    return (int)(unsigned char)ascii_lower(*a) -
           (int)(unsigned char)ascii_lower(*b);
}

char* strchr(const char* s, int c) {
    char target = (char)c;
    while (*s) {
        if (*s == target) return (char*)(uintptr_t)s;
        s++;
    }
    return (target == '\0') ? (char*)(uintptr_t)s : (char*)0;
}

char* strrchr(const char* s, int c) {
    char target = (char)c;
    const char* last = (const char*)0;
    while (*s) {
        if (*s == target) last = s;
        s++;
    }
    if (target == '\0') return (char*)(uintptr_t)s;
    return (char*)(uintptr_t)last;
}

typedef struct {
    char*  buf;
    size_t cap;
    size_t pos;
} fmt_out_t;

static void fmt_putc(fmt_out_t* out, char ch) {
    if (out->buf && out->cap > 0 && out->pos + 1 < out->cap) {
        out->buf[out->pos] = ch;
    }
    out->pos++;
}

static void fmt_puts_pad(fmt_out_t* out, const char* s, int len, int width, int left, char pad) {
    int pad_cnt = (width > len) ? (width - len) : 0;
    if (!left) {
        for (int i = 0; i < pad_cnt; i++) fmt_putc(out, pad);
    }
    for (int i = 0; i < len; i++) fmt_putc(out, s[i]);
    if (left) {
        for (int i = 0; i < pad_cnt; i++) fmt_putc(out, ' ');
    }
}

static void fmt_uint(fmt_out_t* out, uint64_t val, unsigned base, int upper,
                     int neg, int width, int prec, int left, int zero_pad, int alt) {
    char tmp[32];
    int n = 0;
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (val == 0) {
        if (prec != 0) tmp[n++] = '0';
    } else {
        while (val > 0 && n < (int)sizeof(tmp)) {
            tmp[n++] = digits[val % base];
            val /= base;
        }
    }

    int zeros = (prec > n) ? (prec - n) : 0;
    const char* prefix = "";
    int plen = 0;
    if (neg) {
        prefix = "-";
        plen = 1;
    } else if (alt && base == 16 && n > 0) {
        prefix = upper ? "0X" : "0x";
        plen = 2;
    }

    int total_len = plen + zeros + n;
    int pad_cnt = (width > total_len) ? (width - total_len) : 0;

    if (!left && !(zero_pad && prec < 0)) {
        for (int i = 0; i < pad_cnt; i++) fmt_putc(out, ' ');
    }
    for (int i = 0; i < plen; i++) fmt_putc(out, prefix[i]);
    if (!left && zero_pad && prec < 0) {
        for (int i = 0; i < pad_cnt; i++) fmt_putc(out, '0');
    }
    for (int i = 0; i < zeros; i++) fmt_putc(out, '0');
    for (int i = n - 1; i >= 0; i--) fmt_putc(out, tmp[i]);
    if (left) {
        for (int i = 0; i < pad_cnt; i++) fmt_putc(out, ' ');
    }
}

int vsnprintf(char* buf, size_t cap, const char* fmt, va_list ap) {
    fmt_out_t out;
    out.buf = buf;
    out.cap = cap;
    out.pos = 0;

    if (!fmt) {
        if (buf && cap > 0) buf[0] = '\0';
        return 0;
    }

    while (*fmt) {
        if (*fmt != '%') {
            fmt_putc(&out, *fmt++);
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            fmt_putc(&out, '%');
            fmt++;
            continue;
        }

        int left = 0, zero_pad = 0, alt = 0;
        for (;;) {
            if (*fmt == '-') { left = 1; fmt++; }
            else if (*fmt == '0') { zero_pad = 1; fmt++; }
            else if (*fmt == '#') { alt = 1; fmt++; }
            else if (*fmt == '+' || *fmt == ' ') { fmt++; }
            else break;
        }

        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { left = 1; width = -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                prec = 0;
                while (*fmt >= '0' && *fmt <= '9') {
                    prec = prec * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
            if (*fmt == 'l') { is_long = 2; fmt++; }
        } else if (*fmt == 'z' || *fmt == 't') {
            is_long = 2;
            fmt++;
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') fmt++;
        }

        char spec = *fmt;
        if (!spec) break;
        fmt++;

        switch (spec) {
            case 'c': {
                char c = (char)va_arg(ap, int);
                fmt_puts_pad(&out, &c, 1, width, left, ' ');
                break;
            }
            case 's': {
                const char* s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                size_t slen = strlen(s);
                if (prec >= 0 && (size_t)prec < slen) slen = (size_t)prec;
                fmt_puts_pad(&out, s, (int)slen, width, left, ' ');
                break;
            }
            case 'd':
            case 'i': {
                int64_t v = (is_long >= 2) ? va_arg(ap, long long) :
                            (is_long == 1) ? va_arg(ap, long) :
                                             (int64_t)va_arg(ap, int);
                int neg = 0;
                uint64_t uv;
                if (v < 0) {
                    neg = 1;
                    uv = (uint64_t)(-(v + 1)) + 1ULL;
                } else {
                    uv = (uint64_t)v;
                }
                fmt_uint(&out, uv, 10, 0, neg, width, prec, left, zero_pad, 0);
                break;
            }
            case 'u':
            case 'x':
            case 'X': {
                uint64_t uv = (is_long >= 2) ? va_arg(ap, unsigned long long) :
                              (is_long == 1) ? va_arg(ap, unsigned long) :
                                               (uint64_t)va_arg(ap, unsigned int);
                unsigned base = (spec == 'u') ? 10u : 16u;
                int upper = (spec == 'X') ? 1 : 0;
                fmt_uint(&out, uv, base, upper, 0, width, prec, left, zero_pad, alt);
                break;
            }
            case 'p': {
                uint64_t ptr = (uint64_t)(uintptr_t)va_arg(ap, void*);
                fmt_puts_pad(&out, "0x", 2, 0, 0, ' ');
                fmt_uint(&out, ptr, 16, 0, 0, (width > 2) ? (width - 2) : 0,
                         prec < 0 ? 16 : prec, left, 1, 0);
                break;
            }
            default:
                fmt_putc(&out, '%');
                fmt_putc(&out, spec);
                break;
        }
    }

    if (buf && cap > 0) {
        size_t term = (out.pos < cap) ? out.pos : (cap - 1);
        buf[term] = '\0';
    }
    return (int)out.pos;
}

int snprintf(char* buf, size_t cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return rc;
}
