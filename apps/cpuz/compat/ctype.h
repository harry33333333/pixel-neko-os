#ifndef PIXEL_NEKO_LIBCPUID_CTYPE_H
#define PIXEL_NEKO_LIBCPUID_CTYPE_H

static inline int tolower(int value) {
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

static inline int isdigit(int value) {
    return value >= '0' && value <= '9';
}

static inline int isspace(int value) {
    return value == ' ' || value == '\t' || value == '\n' ||
           value == '\r' || value == '\f' || value == '\v';
}

#endif
