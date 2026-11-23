#include "utf8.h"

uint32_t utf8_decode_next(const char** str) {
    if (!str || !*str) return 0u;
    const uint8_t* s = (const uint8_t*)(*str);
    uint8_t b0 = s[0];
    if (b0 == 0u) return 0u;

    if (b0 < 0x80u) {
        *str = (const char*)(s + 1);
        return (uint32_t)b0;
    }

    if ((b0 & 0xE0u) == 0xC0u) {
        uint8_t b1 = s[1];
        if ((b1 & 0xC0u) == 0x80u) {
            uint32_t cp = ((uint32_t)(b0 & 0x1Fu) << 6) |
                          (uint32_t)(b1 & 0x3Fu);
            if (cp >= 0x80u) {
                *str = (const char*)(s + 2);
                return cp;
            }
        }
        *str = (const char*)(s + 1);
        return 0xFFFDu;
    }

    if ((b0 & 0xF0u) == 0xE0u) {
        uint8_t b1 = s[1];
        if ((b1 & 0xC0u) == 0x80u) {
            uint8_t b2 = s[2];
            if ((b2 & 0xC0u) == 0x80u) {
                uint32_t cp = ((uint32_t)(b0 & 0x0Fu) << 12) |
                              ((uint32_t)(b1 & 0x3Fu) << 6) |
                              (uint32_t)(b2 & 0x3Fu);
                if (cp >= 0x800u && (cp < 0xD800u || cp > 0xDFFFu)) {
                    *str = (const char*)(s + 3);
                    return cp;
                }
            }
        }
        *str = (const char*)(s + 1);
        return 0xFFFDu;
    }

    if ((b0 & 0xF8u) == 0xF0u) {
        uint8_t b1 = s[1];
        if ((b1 & 0xC0u) == 0x80u) {
            uint8_t b2 = s[2];
            if ((b2 & 0xC0u) == 0x80u) {
                uint8_t b3 = s[3];
                if ((b3 & 0xC0u) == 0x80u) {
                    uint32_t cp = ((uint32_t)(b0 & 0x07u) << 18) |
                                  ((uint32_t)(b1 & 0x3Fu) << 12) |
                                  ((uint32_t)(b2 & 0x3Fu) << 6) |
                                  (uint32_t)(b3 & 0x3Fu);
                    if (cp >= 0x10000u && cp <= 0x10FFFFu) {
                        *str = (const char*)(s + 4);
                        return cp;
                    }
                }
            }
        }
        *str = (const char*)(s + 1);
        return 0xFFFDu;
    }

    *str = (const char*)(s + 1);
    return 0xFFFDu;
}

int utf8_encode_one(uint32_t cp, char* out) {
    if (!out) return 0;
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        cp = 0xFFFDu;
    }
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

size_t utf16le_to_utf8(const uint16_t* src, size_t max_u16_units,
                       char* dst, size_t dst_cap) {
    if (!dst || dst_cap == 0u) return 0u;
    if (!src) {
        dst[0] = '\0';
        return 0u;
    }

    size_t out_len = 0u;
    size_t i = 0u;
    while (i < max_u16_units) {
        uint16_t u1 = src[i++];
        if (u1 == 0u) break;

        uint32_t cp;
        if (u1 >= 0xD800u && u1 <= 0xDBFFu) {
            if (i < max_u16_units && src[i] >= 0xDC00u && src[i] <= 0xDFFFu) {
                uint16_t u2 = src[i++];
                cp = 0x10000u + (((uint32_t)(u1 - 0xD800u) << 10) |
                                 (uint32_t)(u2 - 0xDC00u));
            } else {
                cp = 0xFFFDu;
            }
        } else if (u1 >= 0xDC00u && u1 <= 0xDFFFu) {
            cp = 0xFFFDu;
        } else {
            cp = (uint32_t)u1;
        }

        char tmp[4];
        int n = utf8_encode_one(cp, tmp);
        if (out_len + (size_t)n >= dst_cap) break;
        for (int k = 0; k < n; k++) {
            dst[out_len++] = tmp[k];
        }
    }
    dst[out_len] = '\0';
    return out_len;
}

size_t utf8_to_utf16le(const char* src, uint16_t* dst, size_t max_u16_units) {
    if (!src) return 0u;
    size_t out_units = 0u;
    const char* p = src;
    while (*p) {
        uint32_t cp = utf8_decode_next(&p);
        if (cp == 0u) break;
        if (cp < 0x10000u) {
            if (dst && out_units >= max_u16_units) break;
            if (dst) dst[out_units] = (uint16_t)cp;
            out_units++;
        } else {
            if (dst && out_units + 1u >= max_u16_units) break;
            uint32_t v = cp - 0x10000u;
            if (dst) {
                dst[out_units]     = (uint16_t)(0xD800u + (v >> 10));
                dst[out_units + 1] = (uint16_t)(0xDC00u + (v & 0x3FFu));
            }
            out_units += 2u;
        }
    }
    return out_units;
}

size_t ucs2be_to_utf8(const uint8_t* src_bytes, size_t byte_len,
                      char* dst, size_t dst_cap) {
    if (!dst || dst_cap == 0u) return 0u;
    if (!src_bytes || byte_len < 2u) {
        dst[0] = '\0';
        return 0u;
    }

    size_t out_len = 0u;
    size_t i = 0u;
    while (i + 1u < byte_len) {
        uint16_t u1 = ((uint16_t)src_bytes[i] << 8) | (uint16_t)src_bytes[i + 1u];
        i += 2u;
        if (u1 == 0u) break;

        uint32_t cp;
        if (u1 >= 0xD800u && u1 <= 0xDBFFu) {
            if (i + 1u < byte_len) {
                uint16_t u2 = ((uint16_t)src_bytes[i] << 8) |
                              (uint16_t)src_bytes[i + 1u];
                if (u2 >= 0xDC00u && u2 <= 0xDFFFu) {
                    i += 2u;
                    cp = 0x10000u + (((uint32_t)(u1 - 0xD800u) << 10) |
                                     (uint32_t)(u2 - 0xDC00u));
                } else {
                    cp = 0xFFFDu;
                }
            } else {
                cp = 0xFFFDu;
            }
        } else if (u1 >= 0xDC00u && u1 <= 0xDFFFu) {
            cp = 0xFFFDu;
        } else {
            cp = (uint32_t)u1;
        }

        char tmp[4];
        int n = utf8_encode_one(cp, tmp);
        if (out_len + (size_t)n >= dst_cap) break;
        for (int k = 0; k < n; k++) {
            dst[out_len++] = tmp[k];
        }
    }
    dst[out_len] = '\0';
    return out_len;
}

int utf8_prev_char_bytes(const char* buf, int cursor_byte_offset) {
    if (!buf || cursor_byte_offset <= 0) return 0;
    int pos = cursor_byte_offset - 1;
    int count = 1;
    while (pos > 0 && count < 4 && (((uint8_t)buf[pos] & 0xC0u) == 0x80u)) {
        pos--;
        count++;
    }
    return count;
}

int utf8_next_char_bytes(const char* buf, int cursor_byte_offset,
                         int total_byte_len) {
    if (!buf || cursor_byte_offset < 0 || cursor_byte_offset >= total_byte_len) {
        return 0;
    }
    uint8_t b0 = (uint8_t)buf[cursor_byte_offset];
    int len = 1;
    if (b0 < 0x80u) len = 1;
    else if ((b0 & 0xE0u) == 0xC0u) len = 2;
    else if ((b0 & 0xF0u) == 0xE0u) len = 3;
    else if ((b0 & 0xF8u) == 0xF0u) len = 4;
    if (cursor_byte_offset + len > total_byte_len) {
        len = total_byte_len - cursor_byte_offset;
    }
    return len;
}

void utf8_clip_by_pixels(const char* src, int max_pixels, int is_small_font,
                         char* dst, size_t dst_cap) {
    if (!dst || dst_cap == 0u) return;
    if (!src || max_pixels <= 0) {
        dst[0] = '\0';
        return;
    }

    int ascii_w = is_small_font ? 6 : 8;
    int cjk_w   = is_small_font ? 12 : 16;
    int ell_w   = ascii_w * 3;

    int total_w = 0;
    const char* p = src;
    while (*p) {
        uint32_t cp = utf8_decode_next(&p);
        if (cp == 0u) break;
        total_w += (cp < 0x80u) ? ascii_w : cjk_w;
    }

    if (total_w <= max_pixels) {
        size_t out = 0u;
        p = src;
        while (*p) {
            const char* prev = p;
            uint32_t cp = utf8_decode_next(&p);
            if (cp == 0u) break;
            size_t clen = (size_t)(p - prev);
            if (out + clen >= dst_cap) break;
            for (size_t k = 0; k < clen; k++) dst[out++] = prev[k];
        }
        dst[out] = '\0';
        return;
    }

    int budget = max_pixels - ell_w;
    size_t out = 0u;
    int cur_w = 0;
    if (budget > 0) {
        p = src;
        while (*p) {
            const char* prev = p;
            uint32_t cp = utf8_decode_next(&p);
            if (cp == 0u) break;
            int cw = (cp < 0x80u) ? ascii_w : cjk_w;
            if (cur_w + cw > budget) break;
            size_t clen = (size_t)(p - prev);
            if (out + clen + 3u >= dst_cap) break;
            for (size_t k = 0; k < clen; k++) dst[out++] = prev[k];
            cur_w += cw;
        }
    }
    for (int d = 0; d < 3 && out + 1u < dst_cap && cur_w + ascii_w <= max_pixels; d++) {
        dst[out++] = '.';
        cur_w += ascii_w;
    }
    dst[out] = '\0';
}

int utf8_validate(const char* buf, size_t len) {
    if (!buf) return len == 0u ? 1 : 0;
    const uint8_t* s = (const uint8_t*)buf;
    size_t i = 0u;
    while (i < len) {
        uint8_t b0 = s[i];
        if (b0 < 0x80u) {
            i++;
        } else if ((b0 & 0xE0u) == 0xC0u) {
            if (i + 1u >= len) return 0;
            uint8_t b1 = s[i + 1u];
            if ((b1 & 0xC0u) != 0x80u) return 0;
            uint32_t cp = ((uint32_t)(b0 & 0x1Fu) << 6) | (uint32_t)(b1 & 0x3Fu);
            if (cp < 0x80u) return 0;
            i += 2u;
        } else if ((b0 & 0xF0u) == 0xE0u) {
            if (i + 2u >= len) return 0;
            uint8_t b1 = s[i + 1u];
            uint8_t b2 = s[i + 2u];
            if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u) return 0;
            uint32_t cp = ((uint32_t)(b0 & 0x0Fu) << 12) |
                          ((uint32_t)(b1 & 0x3Fu) << 6) |
                          (uint32_t)(b2 & 0x3Fu);
            if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu)) return 0;
            i += 3u;
        } else if ((b0 & 0xF8u) == 0xF0u) {
            if (i + 3u >= len) return 0;
            uint8_t b1 = s[i + 1u];
            uint8_t b2 = s[i + 2u];
            uint8_t b3 = s[i + 3u];
            if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u ||
                (b3 & 0xC0u) != 0x80u) {
                return 0;
            }
            uint32_t cp = ((uint32_t)(b0 & 0x07u) << 18) |
                          ((uint32_t)(b1 & 0x3Fu) << 12) |
                          ((uint32_t)(b2 & 0x3Fu) << 6) |
                          (uint32_t)(b3 & 0x3Fu);
            if (cp < 0x10000u || cp > 0x10FFFFu) return 0;
            i += 4u;
        } else {
            return 0;
        }
    }
    return 1;
}
