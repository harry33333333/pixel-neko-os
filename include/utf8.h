#ifndef _UTF8_H
#define _UTF8_H

#include <stddef.h>
#include <stdint.h>

/* Decode next UTF-8 codepoint and advance *str; invalid sequences yield 0xFFFD. */
uint32_t utf8_decode_next(const char** str);

/* Encode a Unicode codepoint into 1..4 UTF-8 bytes; returns byte count written. */
int utf8_encode_one(uint32_t cp, char* out);

/* UTF-16LE code units -> NUL-terminated UTF-8 string; returns UTF-8 byte length. */
size_t utf16le_to_utf8(const uint16_t* src, size_t max_u16_units,
                       char* dst, size_t dst_cap);

/* UTF-8 string -> UTF-16LE code units; returns code unit count written. */
size_t utf8_to_utf16le(const char* src, uint16_t* dst, size_t max_u16_units);

/* UCS-2BE / UTF-16BE byte stream -> NUL-terminated UTF-8 string. */
size_t ucs2be_to_utf8(const uint8_t* src_bytes, size_t byte_len,
                      char* dst, size_t dst_cap);

/* Byte length of the previous/next complete UTF-8 character around cursor. */
int utf8_prev_char_bytes(const char* buf, int cursor_byte_offset);
int utf8_next_char_bytes(const char* buf, int cursor_byte_offset,
                         int total_byte_len);

/* Clip UTF-8 text by pixel width without splitting multi-byte characters. */
void utf8_clip_by_pixels(const char* src, int max_pixels, int is_small_font,
                         char* dst, size_t dst_cap);

/* Validate that buf[0..len) is well-formed UTF-8 (1 = valid, 0 = invalid). */
int utf8_validate(const char* buf, size_t len);

#endif /* _UTF8_H */
