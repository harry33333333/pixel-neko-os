#ifndef _PXE_STR_H
#define _PXE_STR_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

void*  memcpy(void* dst, const void* src, size_t n);
void*  memmove(void* dst, const void* src, size_t n);
void*  memset(void* dst, int c, size_t n);
int    memcmp(const void* a, const void* b, size_t n);

size_t strlen(const char* s);
char*  strcpy(char* dst, const char* src);
char*  strncpy(char* dst, const char* src, size_t n);
char*  strcat(char* dst, const char* src);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
int    strcasecmp(const char* a, const char* b);
char*  strchr(const char* s, int c);
char*  strrchr(const char* s, int c);

int    vsnprintf(char* buf, size_t cap, const char* fmt, va_list ap);
int    snprintf(char* buf, size_t cap, const char* fmt, ...);

/* UTF-8 / UTF-16 helpers */
uint32_t utf8_decode_next(const char** str);
int      utf8_encode_one(uint32_t cp, char* out);
size_t   utf16le_to_utf8(const uint16_t* src, size_t max_u16_units,
                         char* dst, size_t dst_cap);
size_t   utf8_to_utf16le(const char* src, uint16_t* dst, size_t max_u16_units);
size_t   ucs2be_to_utf8(const uint8_t* src_bytes, size_t byte_len,
                        char* dst, size_t dst_cap);
int      utf8_prev_char_bytes(const char* buf, int cursor_byte_offset);
int      utf8_next_char_bytes(const char* buf, int cursor_byte_offset,
                              int total_byte_len);
void     utf8_clip_by_pixels(const char* src, int max_pixels, int is_small_font,
                             char* dst, size_t dst_cap);
int      utf8_validate(const char* buf, size_t len);

#endif /* _PXE_STR_H */

