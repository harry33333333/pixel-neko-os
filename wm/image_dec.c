#include <stdint.h>
#include <stddef.h>
#include "image_dec.h"
#include "ui_memory.h"
#include "vfs.h"

// -------------------------------------------------------------
// Utility functions (independent of libc)
// -------------------------------------------------------------
static void* img_memset(void* s, int c, unsigned int n) {
    unsigned char* p = (unsigned char*)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static void* img_memcpy(void* dest, const void* src, unsigned int n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
    return dest;
}

static void my_str_copy(char* dest, const char* src, int max_len) {
    int i = 0;
    while (src[i] && i < max_len - 1) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

static void my_str_cat(char* dest, const char* src, int max_len) {
    int d = 0;
    while (dest[d] && d < max_len - 1) d++;
    int s = 0;
    while (src[s] && d < max_len - 1) {
        dest[d++] = src[s++];
    }
    dest[d] = '\0';
}

static void my_int_to_str(unsigned int n, char* buf) {
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[16];
    int p = 0;
    while (n > 0) {
        tmp[p++] = '0' + (n % 10);
        n /= 10;
    }
    int d = 0;
    while (p > 0) buf[d++] = tmp[--p];
    buf[d] = '\0';
}

void image_init(image_t* img) {
    if (!img) return;
    img->width = 0;
    img->height = 0;
    img->pixels = 0;
    img->page_count = 0;
}

void image_free(image_t* img) {
    if (!img) return;
    if (img->pixels && img->page_count > 0) {
        pmm_free_pages(img->pixels, img->page_count);
        img->pixels = 0;
        img->page_count = 0;
    }
    img->width = 0;
    img->height = 0;
}

// -------------------------------------------------------------
// Tiny Inflate (tinf) - Embedded lightweight Deflate / zlib decompressor
// -------------------------------------------------------------
typedef struct {
    unsigned short counts[16];
    unsigned short symbols[288];
} tinf_tree;

typedef struct {
    const unsigned char* source;
    unsigned int tag;
    int bitcount;
    tinf_tree ltree; // Literal/length tree
    tinf_tree dtree; // Distance tree
} tinf_data;

// Build Huffman tree from code lengths
static void tinf_build_tree(tinf_tree* t, const unsigned char* lengths, unsigned int num) {
    unsigned short offs[16];
    unsigned int i, sum;

    for (i = 0; i < 16; ++i) t->counts[i] = 0;
    for (i = 0; i < num; ++i) t->counts[lengths[i]]++;
    t->counts[0] = 0;

    for (sum = 0, i = 0; i < 16; ++i) {
        offs[i] = sum;
        sum += t->counts[i];
    }
    for (i = 0; i < num; ++i) {
        if (lengths[i]) {
            t->symbols[offs[lengths[i]]++] = i;
        }
    }
}

// Read bits from input stream
static unsigned int tinf_getbits(tinf_data* d, int num) {
    while (d->bitcount < num) {
        d->tag |= ((unsigned int)*d->source++) << d->bitcount;
        d->bitcount += 8;
    }
    unsigned int val = d->tag & ((1U << num) - 1);
    d->tag >>= num;
    d->bitcount -= num;
    return val;
}

// Decode symbol using Huffman tree
static int tinf_decode_symbol(tinf_data* d, const tinf_tree* t) {
    int base = 0, offs = 0;
    int len;
    for (len = 1; len <= 15; ++len) {
        base = (base << 1) | tinf_getbits(d, 1);
        int count = t->counts[len];
        if (base < count) {
            return t->symbols[offs + base];
        }
        base -= count;
        offs += count;
    }
    return -1;
}

// Given length code, get extra bits and base length
static const unsigned short length_base[30] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 0
};
static const unsigned char length_extra[30] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0, 0
};
static const unsigned short dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const unsigned char dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static void tinf_build_fixed_trees(tinf_tree* lt, tinf_tree* dt) {
    unsigned char lengths[288];
    unsigned int i;
    for (i = 0; i < 144; ++i) lengths[i] = 8;
    for (; i < 256; ++i) lengths[i] = 9;
    for (; i < 280; ++i) lengths[i] = 7;
    for (; i < 288; ++i) lengths[i] = 8;
    tinf_build_tree(lt, lengths, 288);
    for (i = 0; i < 30; ++i) lengths[i] = 5;
    tinf_build_tree(dt, lengths, 30);
}

static int tinf_inflate_block(tinf_data* d, unsigned char* dest, unsigned int* destLen, unsigned int maxLen) {
    while (1) {
        int sym = tinf_decode_symbol(d, &d->ltree);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (*destLen >= maxLen) return -1;
            dest[(*destLen)++] = (unsigned char)sym;
        } else if (sym == 256) {
            return 0; // End of block
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = length_base[sym] + tinf_getbits(d, length_extra[sym]);
            int dist_sym = tinf_decode_symbol(d, &d->dtree);
            if (dist_sym < 0 || dist_sym >= 30) return -1;
            int dist = dist_base[dist_sym] + tinf_getbits(d, dist_extra[dist_sym]);
            if ((int)*destLen < dist) return -1;
            if (*destLen + len > maxLen) return -1;
            const unsigned char* src = dest + *destLen - dist;
            while (len--) {
                dest[(*destLen)++] = *src++;
            }
        }
    }
}

static const unsigned char cl_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static int tinf_uncompress(unsigned char* dest, unsigned int* destLen, const unsigned char* src, unsigned int maxDestLen) {
    tinf_data d;
    d.source = src;
    d.tag = 0;
    d.bitcount = 0;
    *destLen = 0;

    int bfinal;
    do {
        bfinal = tinf_getbits(&d, 1);
        int btype = tinf_getbits(&d, 2);

        if (btype == 0) { // Uncompressed block
            d.tag >>= (d.bitcount & 7);
            d.bitcount -= (d.bitcount & 7);
            unsigned int len = tinf_getbits(&d, 16);
            unsigned int nlen = tinf_getbits(&d, 16);
            if ((len ^ 0xFFFF) != nlen) return -1;
            if (*destLen + len > maxDestLen) return -1;
            while (len--) {
                dest[(*destLen)++] = (unsigned char)tinf_getbits(&d, 8);
            }
        } else if (btype == 1) { // Fixed Huffman
            tinf_build_fixed_trees(&d.ltree, &d.dtree);
            if (tinf_inflate_block(&d, dest, destLen, maxDestLen) < 0) return -1;
        } else if (btype == 2) { // Dynamic Huffman
            int hlit = tinf_getbits(&d, 5) + 257;
            int hdist = tinf_getbits(&d, 5) + 1;
            int hclen = tinf_getbits(&d, 4) + 4;
            unsigned char cl_lengths[19];
            img_memset(cl_lengths, 0, sizeof(cl_lengths));
            for (int i = 0; i < hclen; ++i) {
                cl_lengths[cl_order[i]] = (unsigned char)tinf_getbits(&d, 3);
            }
            tinf_tree cl_tree;
            tinf_build_tree(&cl_tree, cl_lengths, 19);

            unsigned char lengths[288 + 32];
            int num_codes = hlit + hdist;
            int idx = 0;
            while (idx < num_codes) {
                int sym = tinf_decode_symbol(&d, &cl_tree);
                if (sym < 0) return -1;
                if (sym < 16) {
                    lengths[idx++] = (unsigned char)sym;
                } else if (sym == 16) {
                    if (idx == 0) return -1;
                    int rep = tinf_getbits(&d, 2) + 3;
                    unsigned char prev = lengths[idx - 1];
                    while (rep-- && idx < num_codes) lengths[idx++] = prev;
                } else if (sym == 17) {
                    int rep = tinf_getbits(&d, 3) + 3;
                    while (rep-- && idx < num_codes) lengths[idx++] = 0;
                } else if (sym == 18) {
                    int rep = tinf_getbits(&d, 7) + 11;
                    while (rep-- && idx < num_codes) lengths[idx++] = 0;
                }
            }
            tinf_build_tree(&d.ltree, lengths, hlit);
            tinf_build_tree(&d.dtree, lengths + hlit, hdist);
            if (tinf_inflate_block(&d, dest, destLen, maxDestLen) < 0) return -1;
        } else {
            return -1; // Reserved block type
        }
    } while (!bfinal);

    return 0;
}

// -------------------------------------------------------------
// BMP Decoder Implementation
// -------------------------------------------------------------
static int decode_bmp(const unsigned char* data, unsigned int size, image_t* out_img, char* err_msg, int err_msg_len) {
    if (size < 54) {
        my_str_copy(err_msg, "BMP file too small", err_msg_len);
        return -1;
    }
    if (data[0] != 'B' || data[1] != 'M') {
        my_str_copy(err_msg, "Not a valid BMP image", err_msg_len);
        return -1;
    }

    unsigned int bfOffBits = *(unsigned int*)(data + 10);
    int biWidth = *(int*)(data + 18);
    int biHeight = *(int*)(data + 22);
    unsigned short biBitCount = *(unsigned short*)(data + 28);
    unsigned int biCompression = *(unsigned int*)(data + 30);

    if (biWidth <= 0 || biHeight == 0) {
        my_str_copy(err_msg, "Invalid BMP dimensions", err_msg_len);
        return -1;
    }

    int top_down = 0;
    int height = biHeight;
    if (height < 0) {
        top_down = 1;
        height = -height;
    }
    int width = biWidth;

    if (biCompression != 0 && biCompression != 3) {
        my_str_copy(err_msg, "Unsupported BMP compression", err_msg_len);
        return -1;
    }

    // Check memory requirements
    unsigned int total_pixels = (unsigned int)width * (unsigned int)height;
    unsigned int raw_bytes = total_pixels * 4;
    unsigned int pages_needed = (raw_bytes + 4095) / 4096;
    unsigned int free_pages = pmm_get_free_pages();

    if (pages_needed > free_pages) {
        my_str_copy(err_msg, "Out of Memory: Not enough RAM for image", err_msg_len);
        return -2;
    }

    unsigned int* pix = (unsigned int*)pmm_alloc_pages(pages_needed);
    if (!pix) {
        my_str_copy(err_msg, "Out of Memory: PMM allocation failed", err_msg_len);
        return -2;
    }

    out_img->width = width;
    out_img->height = height;
    out_img->pixels = pix;
    out_img->page_count = pages_needed;

    // Palette handling for 1, 4, 8 bpp
    unsigned int palette[256];
    if (biBitCount <= 8) {
        const unsigned char* pal_ptr = data + 14 + *(unsigned int*)(data + 14);
        int colors = 1 << biBitCount;
        for (int i = 0; i < colors; i++) {
            unsigned char b = pal_ptr[i * 4 + 0];
            unsigned char g = pal_ptr[i * 4 + 1];
            unsigned char r = pal_ptr[i * 4 + 2];
            palette[i] = 0xFF000000 | ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
        }
    }

    unsigned int row_bytes = ((width * biBitCount + 31) / 32) * 4;
    const unsigned char* src_bits = data + bfOffBits;

    int has_non_zero_alpha = 0;
    if (biBitCount == 32) {
        for (int y = 0; y < height; y++) {
            const unsigned char* row = src_bits + y * row_bytes;
            for (int x = 0; x < width; x++) {
                if (row[x * 4 + 3] != 0) {
                    has_non_zero_alpha = 1;
                    break;
                }
            }
            if (has_non_zero_alpha) break;
        }
    }

    for (int y = 0; y < height; y++) {
        int src_y = top_down ? y : (height - 1 - y);
        const unsigned char* row = src_bits + src_y * row_bytes;
        unsigned int* dst_row = &pix[y * width];

        if (biBitCount == 32) {
            for (int x = 0; x < width; x++) {
                unsigned char b = row[x * 4 + 0];
                unsigned char g = row[x * 4 + 1];
                unsigned char r = row[x * 4 + 2];
                unsigned char a = has_non_zero_alpha ? row[x * 4 + 3] : 0xFF;
                dst_row[x] = ((unsigned int)a << 24) | ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
            }
        } else if (biBitCount == 24) {
            for (int x = 0; x < width; x++) {
                unsigned char b = row[x * 3 + 0];
                unsigned char g = row[x * 3 + 1];
                unsigned char r = row[x * 3 + 2];
                dst_row[x] = 0xFF000000 | ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
            }
        } else if (biBitCount == 16) {
            for (int x = 0; x < width; x++) {
                unsigned short c = *(unsigned short*)(row + x * 2);
                unsigned char r = (c >> 10) & 0x1F; r = (r << 3) | (r >> 2);
                unsigned char g = (c >> 5) & 0x1F;  g = (g << 3) | (g >> 2);
                unsigned char b = c & 0x1F;         b = (b << 3) | (b >> 2);
                dst_row[x] = 0xFF000000 | ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
            }
        } else if (biBitCount == 8) {
            for (int x = 0; x < width; x++) {
                dst_row[x] = palette[row[x]];
            }
        } else if (biBitCount == 4) {
            for (int x = 0; x < width; x++) {
                unsigned char byte = row[x / 2];
                unsigned char idx = (x & 1) ? (byte & 0x0F) : (byte >> 4);
                dst_row[x] = palette[idx];
            }
        } else if (biBitCount == 1) {
            for (int x = 0; x < width; x++) {
                unsigned char byte = row[x / 8];
                unsigned char idx = (byte >> (7 - (x & 7))) & 1;
                dst_row[x] = palette[idx];
            }
        }
    }

    return 0;
}

// -------------------------------------------------------------
// PNG Decoder Implementation
// -------------------------------------------------------------
static inline int paeth_predict(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static unsigned int read_be32(const unsigned char* p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}

static int decode_png(const unsigned char* data, unsigned int size, image_t* out_img, char* err_msg, int err_msg_len) {
    if (size < 29) {
        my_str_copy(err_msg, "PNG file too small", err_msg_len);
        return -1;
    }
    // Verify 8-byte PNG signature
    if (data[0] != 0x89 || data[1] != 'P' || data[2] != 'N' || data[3] != 'G' ||
        data[4] != 0x0D || data[5] != 0x0A || data[6] != 0x1A || data[7] != 0x0A) {
        my_str_copy(err_msg, "Not a valid PNG file", err_msg_len);
        return -1;
    }

    int width = 0, height = 0;
    int bit_depth = 0, color_type = 0;

    unsigned char palette[256 * 4];
    img_memset(palette, 0xFF, sizeof(palette));
    int has_palette = 0;

    // Scan chunks
    unsigned int pos = 8;
    unsigned int idat_size = 0;
    const unsigned char* first_idat = 0;

    while (pos + 8 <= size) {
        unsigned int chunk_len = read_be32(data + pos);
        unsigned int chunk_type = read_be32(data + pos + 4);
        const unsigned char* chunk_data = data + pos + 8;

        if (pos + 12 + chunk_len > size) break;

        if (chunk_type == 0x49484452) { // 'IHDR'
            width = (int)read_be32(chunk_data);
            height = (int)read_be32(chunk_data + 4);
            bit_depth = chunk_data[8];
            (void)bit_depth;
            color_type = chunk_data[9];
        } else if (chunk_type == 0x504C5445) { // 'PLTE'
            int entries = chunk_len / 3;
            if (entries > 256) entries = 256;
            for (int i = 0; i < entries; i++) {
                palette[i * 4 + 0] = chunk_data[i * 3 + 0]; // R
                palette[i * 4 + 1] = chunk_data[i * 3 + 1]; // G
                palette[i * 4 + 2] = chunk_data[i * 3 + 2]; // B
                palette[i * 4 + 3] = 0xFF;                  // A
            }
            has_palette = 1;
        } else if (chunk_type == 0x74524E53) { // 'tRNS' (Transparency)
            if (color_type == 3 && has_palette) {
                int entries = chunk_len < 256 ? chunk_len : 256;
                for (int i = 0; i < entries; i++) {
                    palette[i * 4 + 3] = chunk_data[i];
                }
            }
        } else if (chunk_type == 0x49444154) { // 'IDAT'
            if (!first_idat) first_idat = chunk_data;
            idat_size += chunk_len;
        } else if (chunk_type == 0x49454E44) { // 'IEND'
            break;
        }

        pos += 12 + chunk_len;
    }

    if (width <= 0 || height <= 0 || !first_idat) {
        my_str_copy(err_msg, "Corrupt PNG metadata or no IDAT", err_msg_len);
        return -1;
    }

    int bytes_per_pixel = 4;
    if (color_type == 0) bytes_per_pixel = 1; // Grayscale
    else if (color_type == 2) bytes_per_pixel = 3; // RGB
    else if (color_type == 3) bytes_per_pixel = 1; // Palette
    else if (color_type == 4) bytes_per_pixel = 2; // Gray + Alpha
    else if (color_type == 6) bytes_per_pixel = 4; // RGBA

    unsigned int stride = 1 + (unsigned int)width * bytes_per_pixel;
    unsigned int raw_decompressed_size = stride * (unsigned int)height;

    // Check available physical memory
    unsigned int out_pages = ((unsigned int)width * (unsigned int)height * 4 + 4095) / 4096;
    unsigned int raw_pages = (raw_decompressed_size + 4095) / 4096;
    unsigned int total_pages_needed = out_pages + raw_pages;

    if (total_pages_needed > pmm_get_free_pages()) {
        unsigned int need_mb = (total_pages_needed * 4) / 1024;
        unsigned int free_mb = (pmm_get_free_pages() * 4) / 1024;
        char nbuf[16], fbuf[16];
        my_int_to_str(need_mb, nbuf);
        my_int_to_str(free_mb, fbuf);
        err_msg[0] = '\0';
        my_str_copy(err_msg, "Out of Memory: Needs ", err_msg_len);
        my_str_cat(err_msg, nbuf, err_msg_len);
        my_str_cat(err_msg, "MB (Free: ", err_msg_len);
        my_str_cat(err_msg, fbuf, err_msg_len);
        my_str_cat(err_msg, "MB)", err_msg_len);
        return -2;
    }

    // Allocate decompressed scanlines buffer
    unsigned char* raw_buf = (unsigned char*)pmm_alloc_pages(raw_pages);
    if (!raw_buf) {
        my_str_copy(err_msg, "Out of Memory: Raw buffer alloc failed", err_msg_len);
        return -2;
    }

    // Combine multiple IDAT chunks if necessary
    const unsigned char* zlib_data = first_idat;
    unsigned char* idat_combined = 0;
    unsigned int idat_pages = 0;

    // Fast check if single IDAT chunk
    pos = 8;
    int idat_count = 0;
    while (pos + 8 <= size) {
        unsigned int chunk_len = read_be32(data + pos);
        unsigned int chunk_type = read_be32(data + pos + 4);
        if (chunk_type == 0x49444154) idat_count++;
        pos += 12 + chunk_len;
    }

    if (idat_count > 1) {
        idat_pages = (idat_size + 4095) / 4096;
        idat_combined = (unsigned char*)pmm_alloc_pages(idat_pages);
        if (idat_combined) {
            unsigned int cpos = 0;
            pos = 8;
            while (pos + 8 <= size) {
                unsigned int chunk_len = read_be32(data + pos);
                unsigned int chunk_type = read_be32(data + pos + 4);
                if (chunk_type == 0x49444154) {
                    img_memcpy(idat_combined + cpos, data + pos + 8, chunk_len);
                    cpos += chunk_len;
                }
                pos += 12 + chunk_len;
            }
            zlib_data = idat_combined;
        }
    }

    // Decompress zlib stream (skip 2-byte header)
    unsigned int decomp_len = 0;
    int ret = tinf_uncompress(raw_buf, &decomp_len, zlib_data + 2, raw_decompressed_size);

    if (idat_combined) {
        pmm_free_pages(idat_combined, idat_pages);
    }

    if (ret != 0 || decomp_len < raw_decompressed_size) {
        pmm_free_pages(raw_buf, raw_pages);
        my_str_copy(err_msg, "PNG decompression failed", err_msg_len);
        return -1;
    }

    // Allocate final 32-bit ARGB image buffer
    unsigned int* pix = (unsigned int*)pmm_alloc_pages(out_pages);
    if (!pix) {
        pmm_free_pages(raw_buf, raw_pages);
        my_str_copy(err_msg, "Out of Memory: Output image buffer failed", err_msg_len);
        return -2;
    }

    out_img->width = width;
    out_img->height = height;
    out_img->pixels = pix;
    out_img->page_count = out_pages;

    // Unfilter scanlines and convert to 32-bit ARGB
    const unsigned char* prev_row = 0;

    for (int y = 0; y < height; y++) {
        unsigned char* cur_row = raw_buf + (unsigned int)y * stride;
        int filter_type = cur_row[0];
        unsigned char* cur_pixels = cur_row + 1;

        if (filter_type == 1) { // Sub
            for (int i = bytes_per_pixel; i < width * bytes_per_pixel; i++) {
                cur_pixels[i] += cur_pixels[i - bytes_per_pixel];
            }
        } else if (filter_type == 2) { // Up
            if (prev_row) {
                for (int i = 0; i < width * bytes_per_pixel; i++) {
                    cur_pixels[i] += prev_row[i];
                }
            }
        } else if (filter_type == 3) { // Average
            for (int i = 0; i < width * bytes_per_pixel; i++) {
                int a = (i >= bytes_per_pixel) ? cur_pixels[i - bytes_per_pixel] : 0;
                int b = prev_row ? prev_row[i] : 0;
                cur_pixels[i] += (unsigned char)((a + b) >> 1);
            }
        } else if (filter_type == 4) { // Paeth
            for (int i = 0; i < width * bytes_per_pixel; i++) {
                int a = (i >= bytes_per_pixel) ? cur_pixels[i - bytes_per_pixel] : 0;
                int b = prev_row ? prev_row[i] : 0;
                int c = (prev_row && i >= bytes_per_pixel) ? prev_row[i - bytes_per_pixel] : 0;
                cur_pixels[i] += (unsigned char)paeth_predict(a, b, c);
            }
        }

        prev_row = cur_pixels;

        // Convert scanline to 32-bit ARGB
        unsigned int* dst_row = &pix[y * width];
        if (color_type == 6) { // RGBA
            for (int x = 0; x < width; x++) {
                unsigned char r = cur_pixels[x * 4 + 0];
                unsigned char g = cur_pixels[x * 4 + 1];
                unsigned char b = cur_pixels[x * 4 + 2];
                unsigned char a = cur_pixels[x * 4 + 3];
                dst_row[x] = ((unsigned int)a << 24) | ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
            }
        } else if (color_type == 2) { // RGB
            for (int x = 0; x < width; x++) {
                unsigned char r = cur_pixels[x * 3 + 0];
                unsigned char g = cur_pixels[x * 3 + 1];
                unsigned char b = cur_pixels[x * 3 + 2];
                dst_row[x] = 0xFF000000 | ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
            }
        } else if (color_type == 3) { // Palette
            for (int x = 0; x < width; x++) {
                unsigned char idx = cur_pixels[x];
                unsigned char r = palette[idx * 4 + 0];
                unsigned char g = palette[idx * 4 + 1];
                unsigned char b = palette[idx * 4 + 2];
                unsigned char a = palette[idx * 4 + 3];
                dst_row[x] = ((unsigned int)a << 24) | ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
            }
        } else if (color_type == 0) { // Grayscale
            for (int x = 0; x < width; x++) {
                unsigned char v = cur_pixels[x];
                dst_row[x] = 0xFF000000 | ((unsigned int)v << 16) | ((unsigned int)v << 8) | (unsigned int)v;
            }
        } else if (color_type == 4) { // Gray + Alpha
            for (int x = 0; x < width; x++) {
                unsigned char v = cur_pixels[x * 2 + 0];
                unsigned char a = cur_pixels[x * 2 + 1];
                dst_row[x] = ((unsigned int)a << 24) | ((unsigned int)v << 16) | ((unsigned int)v << 8) | (unsigned int)v;
            }
        }
    }

    pmm_free_pages(raw_buf, raw_pages);
    return 0;
}

// -------------------------------------------------------------
// JPEG Decoder Implementation (SOF0 / SOF1 / SOF2, Integer Only)
// -------------------------------------------------------------
static const unsigned char k_kjpg_zz[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

typedef struct {
    int            valid;
    int32_t        maxcode[17];
    int32_t        valoffset[17];
    unsigned char  vals[256];
    uint16_t       lut[256];
} kjpg_huff_t;

typedef struct {
    unsigned char  id;
    unsigned char  h;
    unsigned char  v;
    unsigned char  qsel;
    int            bw;
    int            bh;
    int16_t*       coeffs;
    unsigned int   coeff_pages;
} kjpg_comp_t;

typedef struct {
    const unsigned char* src;
    size_t               pos;
    size_t               end;
    uint64_t             bitbuf;
    int                  bitcnt;
    unsigned char        marker;
} kjpg_br_t;

typedef struct {
    uint16_t      qtab[4][64];
    int           qvalid[4];
    kjpg_huff_t   dc_huff[4];
    kjpg_huff_t   ac_huff[4];
    kjpg_comp_t   comp[4];
    int           width;
    int           height;
    int           ncomp;
    int           max_h;
    int           max_v;
    int           mcu_cols;
    int           mcu_rows;
    int           progressive;
    int           dri;
    int           has_jfif;
    int           has_adobe;
    unsigned char adobe_xform;
    unsigned char mcu_px[4][16][64];
} kjpg_ctx_t;

static uint16_t kjpg_be16(const unsigned char* p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static int kjpg_eq_tag(const unsigned char* p, const char* tag, int n) {
    for (int i = 0; i < n; i++) {
        if (p[i] != (unsigned char)tag[i]) return 0;
    }
    return 1;
}

static unsigned char kjpg_clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (unsigned char)v;
}

static int kjpg_build_huff(kjpg_huff_t* h, const unsigned char bits[16],
                           const unsigned char* vals, int nvals) {
    img_memset(h, 0, sizeof(*h));
    if (nvals > 256) return -1;
    img_memcpy(h->vals, vals, (unsigned int)nvals);
    int code = 0;
    int vi = 0;
    for (int len = 1; len <= 16; len++) {
        int cnt = bits[len - 1];
        if (cnt == 0) {
            h->maxcode[len] = -1;
        } else {
            if (vi + cnt > nvals) return -1;
            h->valoffset[len] = vi - code;
            for (int k = 0; k < cnt; k++) {
                unsigned char sym = vals[vi++];
                if (len <= 8) {
                    int shift = 8 - len;
                    int base = code << shift;
                    int span = 1 << shift;
                    for (int m = 0; m < span; m++) {
                        if (base + m < 256) {
                            h->lut[base + m] = (uint16_t)(0x8000u | ((uint16_t)len << 8) | sym);
                        }
                    }
                }
                code++;
            }
            h->maxcode[len] = code - 1;
        }
        code <<= 1;
    }
    h->valid = 1;
    return 0;
}

static void kjpg_fill_bits(kjpg_br_t* br) {
    while (br->bitcnt <= 32) {
        if (br->marker || br->pos >= br->end) break;
        unsigned char b = br->src[br->pos++];
        if (b == 0xFFu) {
            while (br->pos < br->end && br->src[br->pos] == 0xFFu) br->pos++;
            if (br->pos >= br->end) {
                br->marker = 0xD9u;
                break;
            }
            unsigned char b2 = br->src[br->pos++];
            if (b2 != 0x00u) {
                br->marker = b2;
                break;
            }
        }
        br->bitbuf = (br->bitbuf << 8) | (uint64_t)b;
        br->bitcnt += 8;
    }
}

static int kjpg_get_bit(kjpg_br_t* br) {
    if (br->bitcnt == 0) {
        kjpg_fill_bits(br);
        if (br->bitcnt == 0) return -1;
    }
    br->bitcnt--;
    return (int)((br->bitbuf >> br->bitcnt) & 1ULL);
}

static int kjpg_get_bits(kjpg_br_t* br, int n) {
    if (n == 0) return 0;
    if (br->bitcnt < n) kjpg_fill_bits(br);
    if (br->bitcnt < n) return -1;
    br->bitcnt -= n;
    return (int)((br->bitbuf >> br->bitcnt) & ((1ULL << n) - 1ULL));
}

static int kjpg_huff_decode(kjpg_br_t* br, const kjpg_huff_t* h) {
    if (br->bitcnt < 16) kjpg_fill_bits(br);
    if (br->bitcnt >= 8) {
        unsigned char idx8 = (unsigned char)(br->bitbuf >> (br->bitcnt - 8));
        uint16_t e = h->lut[idx8];
        if (e & 0x8000u) {
            br->bitcnt -= (int)((e >> 8) & 0x0Fu);
            return (int)(e & 0xFFu);
        }
    }
    int code = 0;
    for (int len = 1; len <= 16; len++) {
        int b = kjpg_get_bit(br);
        if (b < 0) return -1;
        code = (code << 1) | b;
        if (code <= h->maxcode[len]) {
            int idx = code + h->valoffset[len];
            if (idx < 0 || idx >= 256) return -1;
            return (int)h->vals[idx];
        }
    }
    return -1;
}

static int kjpg_extend(int v, int s) {
    if (s == 0) return 0;
    int vt = 1 << (s - 1);
    if (v < vt) return v + ((-1) << s) + 1;
    return v;
}

static void kjpg_idct_8x8(const int32_t coeff[64], unsigned char out[64]) {
    int64_t ws[64];
    for (int y = 0; y < 8; y++) {
        const int32_t* b = coeff + y * 8;
        int64_t e0 = 1448LL * ((int64_t)b[0] + (int64_t)b[4]);
        int64_t e1 = 1448LL * ((int64_t)b[0] - (int64_t)b[4]);
        int64_t e2 = 1892LL * (int64_t)b[2] + 784LL * (int64_t)b[6];
        int64_t e3 = 784LL * (int64_t)b[2] - 1892LL * (int64_t)b[6];
        int64_t o0 = 2009LL * b[1] + 1703LL * b[3] + 1138LL * b[5] + 400LL * b[7];
        int64_t o1 = 1703LL * b[1] - 400LL * b[3] - 2009LL * b[5] - 1138LL * b[7];
        int64_t o2 = 1138LL * b[1] - 2009LL * b[3] + 400LL * b[5] + 1703LL * b[7];
        int64_t o3 = 400LL * b[1] - 1138LL * b[3] + 1703LL * b[5] - 2009LL * b[7];
        int64_t es[4] = { e0 + e2, e1 + e3, e1 - e3, e0 - e2 };
        int64_t os[4] = { o0, o1, o2, o3 };
        for (int x = 0; x < 4; x++) {
            ws[y * 8 + x]     = (es[x] + os[x] + 128LL) >> 8;
            ws[y * 8 + 7 - x] = (es[x] - os[x] + 128LL) >> 8;
        }
    }
    for (int x = 0; x < 8; x++) {
        int64_t b0 = ws[0 * 8 + x], b1 = ws[1 * 8 + x];
        int64_t b2 = ws[2 * 8 + x], b3 = ws[3 * 8 + x];
        int64_t b4 = ws[4 * 8 + x], b5 = ws[5 * 8 + x];
        int64_t b6 = ws[6 * 8 + x], b7 = ws[7 * 8 + x];
        int64_t e0 = 1448LL * (b0 + b4);
        int64_t e1 = 1448LL * (b0 - b4);
        int64_t e2 = 1892LL * b2 + 784LL * b6;
        int64_t e3 = 784LL * b2 - 1892LL * b6;
        int64_t o0 = 2009LL * b1 + 1703LL * b3 + 1138LL * b5 + 400LL * b7;
        int64_t o1 = 1703LL * b1 - 400LL * b3 - 2009LL * b5 - 1138LL * b7;
        int64_t o2 = 1138LL * b1 - 2009LL * b3 + 400LL * b5 + 1703LL * b7;
        int64_t o3 = 400LL * b1 - 1138LL * b3 + 1703LL * b5 - 2009LL * b7;
        int64_t es[4] = { e0 + e2, e1 + e3, e1 - e3, e0 - e2 };
        int64_t os[4] = { o0, o1, o2, o3 };
        for (int y = 0; y < 4; y++) {
            int p0 = (int)((es[y] + os[y] + 32768LL) >> 16) + 128;
            int p1 = (int)((es[y] - os[y] + 32768LL) >> 16) + 128;
            out[y * 8 + x]       = kjpg_clamp8(p0);
            out[(7 - y) * 8 + x] = kjpg_clamp8(p1);
        }
    }
}

static void kjpg_check_restart(kjpg_br_t* br, int dri, int* mcus_left,
                               int dc_pred[4], int* eobrun) {
    if (dri <= 0) return;
    if (--(*mcus_left) > 0) return;
    br->bitcnt = 0;
    if (br->marker >= 0xD0u && br->marker <= 0xD7u) {
        br->marker = 0;
    } else if (br->marker == 0) {
        while (br->pos < br->end) {
            if (br->src[br->pos] == 0xFFu && br->pos + 1 < br->end) {
                unsigned char nb = br->src[br->pos + 1];
                if (nb >= 0xD0u && nb <= 0xD7u) {
                    br->pos += 2;
                    break;
                } else if (nb != 0x00u && nb != 0xFFu) {
                    break;
                }
            }
            br->pos++;
        }
    }
    dc_pred[0] = dc_pred[1] = dc_pred[2] = dc_pred[3] = 0;
    *eobrun = 0;
    *mcus_left = dri;
}

static int kjpg_decode_block_direct(kjpg_br_t* br, const kjpg_huff_t* dc_h,
                                    const kjpg_huff_t* ac_h, const uint16_t qt[64],
                                    int* dc_pred, int32_t out32[64]) {
    img_memset(out32, 0, sizeof(int32_t) * 64u);
    int s = kjpg_huff_decode(br, dc_h);
    if (s < 0 || s > 15) return -1;
    int v = (s > 0) ? kjpg_get_bits(br, s) : 0;
    if (v < 0) return -1;
    *dc_pred += kjpg_extend(v, s);
    out32[0] = (int32_t)(*dc_pred) * (int32_t)qt[0];

    int k = 1;
    while (k < 64) {
        int rs = kjpg_huff_decode(br, ac_h);
        if (rs < 0) return -1;
        int r = rs >> 4;
        int sz = rs & 0x0F;
        if (sz == 0) {
            if (r == 15) k += 16;
            else break;
        } else {
            k += r;
            if (k >= 64) return -1;
            int ac = kjpg_get_bits(br, sz);
            if (ac < 0) return -1;
            out32[k_kjpg_zz[k]] = (int32_t)kjpg_extend(ac, sz) * (int32_t)qt[k];
            k++;
        }
    }
    return 0;
}

static int kjpg_decode_scan_block(kjpg_ctx_t* j, kjpg_br_t* br, int16_t* blk,
                                  int td, int ta, int* dc_pred, int* eobrun,
                                  int prog, int ss, int se, int ah, int al) {
    if (!prog) {
        int s = kjpg_huff_decode(br, &j->dc_huff[td]);
        if (s < 0 || s > 15) return -1;
        int v = (s > 0) ? kjpg_get_bits(br, s) : 0;
        if (v < 0) return -1;
        *dc_pred += kjpg_extend(v, s);
        blk[0] = (int16_t)(*dc_pred);
        int k = 1;
        while (k < 64) {
            int rs = kjpg_huff_decode(br, &j->ac_huff[ta]);
            if (rs < 0) return -1;
            int r = rs >> 4;
            int sz = rs & 0x0F;
            if (sz == 0) {
                if (r == 15) k += 16;
                else break;
            } else {
                k += r;
                if (k >= 64) return -1;
                int ac = kjpg_get_bits(br, sz);
                if (ac < 0) return -1;
                blk[k] = (int16_t)kjpg_extend(ac, sz);
                k++;
            }
        }
        return 0;
    }

    if (ss == 0 && se == 0) {
        if (ah == 0) {
            int s = kjpg_huff_decode(br, &j->dc_huff[td]);
            if (s < 0 || s > 15) return -1;
            int v = (s > 0) ? kjpg_get_bits(br, s) : 0;
            if (v < 0) return -1;
            *dc_pred += kjpg_extend(v, s);
            blk[0] = (int16_t)(*dc_pred << al);
        } else {
            int bit = kjpg_get_bit(br);
            if (bit < 0) return -1;
            if (bit) blk[0] = (int16_t)(blk[0] | (1 << al));
        }
        return 0;
    }

    if (ah == 0) {
        if (*eobrun > 0) {
            (*eobrun)--;
            return 0;
        }
        int k = ss;
        while (k <= se) {
            int rs = kjpg_huff_decode(br, &j->ac_huff[ta]);
            if (rs < 0) return -1;
            int r = rs >> 4;
            int sz = rs & 0x0F;
            if (sz == 0) {
                if (r == 15) {
                    k += 16;
                } else {
                    int ext = (r > 0) ? kjpg_get_bits(br, r) : 0;
                    if (ext < 0) return -1;
                    *eobrun = (1 << r) + ext - 1;
                    break;
                }
            } else {
                k += r;
                if (k > se) return -1;
                int ac = kjpg_get_bits(br, sz);
                if (ac < 0) return -1;
                blk[k] = (int16_t)(kjpg_extend(ac, sz) << al);
                k++;
            }
        }
        return 0;
    }

    int p1 = 1 << al;
    int m1 = -p1;
    int k = ss;
    if (*eobrun == 0) {
        while (k <= se) {
            int rs = kjpg_huff_decode(br, &j->ac_huff[ta]);
            if (rs < 0) return -1;
            int r = rs >> 4;
            int sz = rs & 0x0F;
            int val = 0;
            if (sz == 0) {
                if (r < 15) {
                    int ext = (r > 0) ? kjpg_get_bits(br, r) : 0;
                    if (ext < 0) return -1;
                    *eobrun = (1 << r) + ext;
                    break;
                }
            } else {
                if (sz != 1) return -1;
                int sbit = kjpg_get_bit(br);
                if (sbit < 0) return -1;
                val = sbit ? p1 : m1;
            }
            while (k <= se) {
                if (blk[k] != 0) {
                    int rbit = kjpg_get_bit(br);
                    if (rbit < 0) return -1;
                    int mag = (blk[k] < 0) ? -blk[k] : blk[k];
                    if (rbit && (mag & p1) == 0) {
                        blk[k] = (int16_t)(blk[k] >= 0 ? blk[k] + p1 : blk[k] + m1);
                    }
                } else {
                    if (r == 0) break;
                    r--;
                }
                k++;
            }
            if (sz != 0 && k <= se) blk[k] = (int16_t)val;
            k++;
        }
    }
    if (*eobrun > 0) {
        while (k <= se) {
            if (blk[k] != 0) {
                int rbit = kjpg_get_bit(br);
                if (rbit < 0) return -1;
                int mag = (blk[k] < 0) ? -blk[k] : blk[k];
                if (rbit && (mag & p1) == 0) {
                    blk[k] = (int16_t)(blk[k] >= 0 ? blk[k] + p1 : blk[k] + m1);
                }
            }
            k++;
        }
        (*eobrun)--;
    }
    return 0;
}

static void kjpg_write_mcu(const kjpg_ctx_t* j, unsigned int* pix, int mx, int my) {
    int mcu_w = j->max_h * 8;
    int mcu_h = j->max_v * 8;
    int x0 = mx * mcu_w;
    int y0 = my * mcu_h;
    int x1 = x0 + mcu_w; if (x1 > j->width)  x1 = j->width;
    int y1 = y0 + mcu_h; if (y1 > j->height) y1 = j->height;

    int is_rgb = (j->ncomp == 3) &&
                 ((j->has_adobe && j->adobe_xform == 0) ||
                  (!j->has_jfif && j->comp[0].id == 'R' &&
                   j->comp[1].id == 'G' && j->comp[2].id == 'B'));

    for (int y = y0; y < y1; y++) {
        int ly = y - y0;
        unsigned int* dst = pix + (size_t)y * (size_t)j->width;
        for (int x = x0; x < x1; x++) {
            int lx = x - x0;
            unsigned char s[4] = { 0, 128, 128, 255 };
            for (int c = 0; c < j->ncomp; c++) {
                int cx = (lx * j->comp[c].h) / j->max_h;
                int cy = (ly * j->comp[c].v) / j->max_v;
                int blk_idx = (cy >> 3) * j->comp[c].h + (cx >> 3);
                s[c] = j->mcu_px[c][blk_idx][(cy & 7) * 8 + (cx & 7)];
            }
            if (j->ncomp == 1) {
                unsigned char v = s[0];
                dst[x] = 0xFF000000u | ((unsigned int)v << 16) | ((unsigned int)v << 8) | v;
            } else if (j->ncomp == 3) {
                if (is_rgb) {
                    dst[x] = 0xFF000000u | ((unsigned int)s[0] << 16) |
                             ((unsigned int)s[1] << 8) | s[2];
                } else {
                    int yv = (int)s[0];
                    int cb = (int)s[1] - 128;
                    int cr = (int)s[2] - 128;
                    unsigned char r = kjpg_clamp8(yv + ((91881 * cr + 32768) >> 16));
                    unsigned char g = kjpg_clamp8(yv - ((22554 * cb + 46802 * cr + 32768) >> 16));
                    unsigned char b = kjpg_clamp8(yv + ((116130 * cb + 32768) >> 16));
                    dst[x] = 0xFF000000u | ((unsigned int)r << 16) | ((unsigned int)g << 8) | b;
                }
            } else {
                int c0 = s[0], c1 = s[1], c2 = s[2], k = s[3];
                if (j->has_adobe && j->adobe_xform == 2) {
                    int yv = c0, cb = c1 - 128, cr = c2 - 128;
                    c0 = kjpg_clamp8(yv + ((91881 * cr + 32768) >> 16));
                    c1 = kjpg_clamp8(yv - ((22554 * cb + 46802 * cr + 32768) >> 16));
                    c2 = kjpg_clamp8(yv + ((116130 * cb + 32768) >> 16));
                }
                unsigned char r = (unsigned char)((c0 * k) / 255);
                unsigned char g = (unsigned char)((c1 * k) / 255);
                unsigned char b = (unsigned char)((c2 * k) / 255);
                dst[x] = 0xFF000000u | ((unsigned int)r << 16) | ((unsigned int)g << 8) | b;
            }
        }
    }
}

static int decode_jpg(const unsigned char* data, unsigned int size, image_t* out_img,
                      char* err_msg, int err_msg_len) {
    if (size < 4 || data[0] != 0xFFu || data[1] != 0xD8u) {
        my_str_copy(err_msg, "Not a valid JPEG image", err_msg_len);
        return -1;
    }

    unsigned int ctx_pages = (unsigned int)((sizeof(kjpg_ctx_t) + 4095u) / 4096u);
    kjpg_ctx_t* j = (kjpg_ctx_t*)pmm_alloc_pages(ctx_pages);
    if (!j) {
        my_str_copy(err_msg, "Out of Memory for JPEG context", err_msg_len);
        return -2;
    }
    img_memset(j, 0, ctx_pages * 4096u);

    unsigned int* pix = 0;
    unsigned int out_pages = 0;
    int rendered_direct = 0;
    size_t pos = 2;
    unsigned char next_m = 0;
    int rc = 0;

    while (1) {
        unsigned char m = 0;
        if (next_m) {
            m = next_m;
            next_m = 0;
        } else {
            while (pos < (size_t)size && data[pos] != 0xFFu) pos++;
            while (pos < (size_t)size && data[pos] == 0xFFu) pos++;
            if (pos >= (size_t)size) break;
            m = data[pos++];
            if (m == 0x00u || (m >= 0xD0u && m <= 0xD7u) || m == 0xD8u || m == 0x01u) continue;
        }

        if (m == 0xD9u) break;
        if (pos + 2 > (size_t)size) break;
        uint16_t seg_len = kjpg_be16(data + pos);
        if (seg_len < 2 || pos + seg_len > (size_t)size) {
            my_str_copy(err_msg, "Corrupt JPEG segment", err_msg_len);
            rc = -1;
            break;
        }
        const unsigned char* seg = data + pos + 2;
        size_t slen = (size_t)(seg_len - 2u);
        pos += seg_len;

        if (m == 0xDBu) {
            size_t i = 0;
            while (i < slen) {
                unsigned char pqtq = seg[i++];
                int pq = pqtq >> 4;
                int tq = pqtq & 0x0F;
                if (tq >= 4 || (pq != 0 && pq != 1)) { rc = -1; break; }
                size_t need = pq ? 128u : 64u;
                if (i + need > slen) { rc = -1; break; }
                for (int k = 0; k < 64; k++) {
                    j->qtab[tq][k] = pq ? kjpg_be16(seg + i + k * 2) : (uint16_t)seg[i + k];
                }
                i += need;
                j->qvalid[tq] = 1;
            }
            if (rc != 0) { my_str_copy(err_msg, "Invalid JPEG DQT", err_msg_len); break; }
        } else if (m == 0xC0u || m == 0xC1u || m == 0xC2u) {
            if (slen < 6 || seg[0] != 8) {
                my_str_copy(err_msg, "Unsupported JPEG frame format", err_msg_len);
                rc = -1;
                break;
            }
            j->progressive = (m == 0xC2u);
            j->height = (int)kjpg_be16(seg + 1);
            j->width  = (int)kjpg_be16(seg + 3);
            j->ncomp  = (int)seg[5];
            if (j->width <= 0 || j->height <= 0 || j->width > 8192 || j->height > 8192 ||
                (j->ncomp != 1 && j->ncomp != 3 && j->ncomp != 4) ||
                slen < (size_t)(6 + 3 * j->ncomp)) {
                my_str_copy(err_msg, "JPEG dimensions/components unsupported", err_msg_len);
                rc = -1;
                break;
            }
            j->max_h = 1;
            j->max_v = 1;
            for (int c = 0; c < j->ncomp; c++) {
                j->comp[c].id   = seg[6 + 3 * c];
                unsigned char hv = seg[7 + 3 * c];
                j->comp[c].h    = (unsigned char)(hv >> 4);
                j->comp[c].v    = (unsigned char)(hv & 0x0F);
                j->comp[c].qsel = (unsigned char)(seg[8 + 3 * c] & 0x03);
                if (j->comp[c].h < 1 || j->comp[c].h > 4 ||
                    j->comp[c].v < 1 || j->comp[c].v > 4) {
                    rc = -1;
                    break;
                }
                if (j->comp[c].h > j->max_h) j->max_h = j->comp[c].h;
                if (j->comp[c].v > j->max_v) j->max_v = j->comp[c].v;
            }
            if (rc != 0) { my_str_copy(err_msg, "Invalid JPEG sampling", err_msg_len); break; }
            int mcu_w = j->max_h * 8;
            int mcu_h = j->max_v * 8;
            j->mcu_cols = (j->width  + mcu_w - 1) / mcu_w;
            j->mcu_rows = (j->height + mcu_h - 1) / mcu_h;
            for (int c = 0; c < j->ncomp; c++) {
                j->comp[c].bw = j->mcu_cols * j->comp[c].h;
                j->comp[c].bh = j->mcu_rows * j->comp[c].v;
                if (j->progressive) {
                    size_t bytes = (size_t)j->comp[c].bw * (size_t)j->comp[c].bh * 64u * sizeof(int16_t);
                    unsigned int pg = (unsigned int)((bytes + 4095u) / 4096u);
                    if (pg > pmm_get_free_pages()) { rc = -2; break; }
                    j->comp[c].coeffs = (int16_t*)pmm_alloc_pages(pg);
                    if (!j->comp[c].coeffs) { rc = -2; break; }
                    j->comp[c].coeff_pages = pg;
                    img_memset(j->comp[c].coeffs, 0, pg * 4096u);
                }
            }
            if (rc != 0) { my_str_copy(err_msg, "Out of Memory decoding JPEG", err_msg_len); break; }
        } else if (m == 0xC4u) {
            size_t i = 0;
            while (i < slen) {
                if (i + 17 > slen) { rc = -1; break; }
                unsigned char tcth = seg[i++];
                int tc = tcth >> 4;
                int th = tcth & 0x0F;
                if (tc > 1 || th >= 4) { rc = -1; break; }
                const unsigned char* bits = seg + i;
                i += 16;
                int nvals = 0;
                for (int b = 0; b < 16; b++) nvals += bits[b];
                if (i + (size_t)nvals > slen) { rc = -1; break; }
                kjpg_huff_t* tbl = tc ? &j->ac_huff[th] : &j->dc_huff[th];
                if (kjpg_build_huff(tbl, bits, seg + i, nvals) != 0) { rc = -1; break; }
                i += (size_t)nvals;
            }
            if (rc != 0) { my_str_copy(err_msg, "Invalid JPEG DHT", err_msg_len); break; }
        } else if (m == 0xDDu) {
            if (slen >= 2) j->dri = (int)kjpg_be16(seg);
        } else if (m == 0xE0u && slen >= 5 && kjpg_eq_tag(seg, "JFIF", 4)) {
            j->has_jfif = 1;
        } else if (m == 0xEEu && slen >= 12 && kjpg_eq_tag(seg, "Adobe", 5)) {
            j->has_adobe = 1;
            j->adobe_xform = seg[11];
        } else if (m == 0xDAu) {
            if (j->width <= 0 || j->ncomp <= 0 || slen < 1) {
                my_str_copy(err_msg, "JPEG SOS before SOF", err_msg_len);
                rc = -1;
                break;
            }
            int ns = (int)seg[0];
            if (ns < 1 || ns > j->ncomp || slen < (size_t)(4 + 2 * ns)) {
                my_str_copy(err_msg, "Invalid JPEG SOS", err_msg_len);
                rc = -1;
                break;
            }
            int scan_c[4], scan_td[4], scan_ta[4];
            for (int k = 0; k < ns; k++) {
                unsigned char cs = seg[1 + 2 * k];
                unsigned char tdta = seg[2 + 2 * k];
                scan_td[k] = (int)(tdta >> 4);
                scan_ta[k] = (int)(tdta & 0x0F);
                int found = -1;
                for (int c = 0; c < j->ncomp; c++) {
                    if (j->comp[c].id == cs) { found = c; break; }
                }
                if (found < 0 || scan_td[k] >= 4 || scan_ta[k] >= 4) { rc = -1; break; }
                scan_c[k] = found;
            }
            if (rc != 0) { my_str_copy(err_msg, "Invalid JPEG SOS comp", err_msg_len); break; }

            int ss = (int)seg[1 + 2 * ns];
            int se = (int)seg[2 + 2 * ns];
            unsigned char ahal = seg[3 + 2 * ns];
            int ah = (int)(ahal >> 4);
            int al = (int)(ahal & 0x0F);
            if (ss > se || se >= 64) { rc = -1; break; }
            for (int k = 0; k < ns; k++) {
                if ((ss == 0 && ah == 0 && !j->dc_huff[scan_td[k]].valid) ||
                    (se > 0 && !j->ac_huff[scan_ta[k]].valid) ||
                    !j->qvalid[j->comp[scan_c[k]].qsel]) {
                    rc = -1;
                    break;
                }
            }
            if (rc != 0) { my_str_copy(err_msg, "Missing JPEG table", err_msg_len); break; }

            kjpg_br_t br;
            img_memset(&br, 0, sizeof(br));
            br.src = data;
            br.pos = pos;
            br.end = (size_t)size;
            int dc_pred[4] = { 0, 0, 0, 0 };
            int eobrun = 0;
            int mcus_left = j->dri;

            if (!j->progressive && ns == j->ncomp && !rendered_direct) {
                if (!pix) {
                    uint64_t npix = (uint64_t)j->width * (uint64_t)j->height * 4ull;
                    out_pages = (unsigned int)((npix + 4095ull) / 4096ull);
                    if (out_pages > pmm_get_free_pages()) {
                        my_str_copy(err_msg, "Out of Memory for image", err_msg_len);
                        rc = -2;
                        break;
                    }
                    pix = (unsigned int*)pmm_alloc_pages(out_pages);
                    if (!pix) {
                        my_str_copy(err_msg, "Out of Memory for image", err_msg_len);
                        rc = -2;
                        break;
                    }
                }
                size_t total_mcus = (size_t)j->mcu_cols * (size_t)j->mcu_rows;
                size_t mcu_idx = 0;
                for (int my = 0; my < j->mcu_rows && rc == 0; my++) {
                    for (int mx = 0; mx < j->mcu_cols && rc == 0; mx++, mcu_idx++) {
                        for (int k = 0; k < ns && rc == 0; k++) {
                            int ci = scan_c[k];
                            kjpg_comp_t* c = &j->comp[ci];
                            for (int v = 0; v < c->v && rc == 0; v++) {
                                for (int u = 0; u < c->h && rc == 0; u++) {
                                    int32_t blk32[64];
                                    if (kjpg_decode_block_direct(&br, &j->dc_huff[scan_td[k]],
                                                                 &j->ac_huff[scan_ta[k]],
                                                                 j->qtab[c->qsel],
                                                                 &dc_pred[k], blk32) != 0) {
                                        my_str_copy(err_msg, "JPEG decode error", err_msg_len);
                                        rc = -1;
                                        break;
                                    }
                                    kjpg_idct_8x8(blk32, j->mcu_px[ci][v * c->h + u]);
                                }
                            }
                        }
                        if (rc == 0) {
                            kjpg_write_mcu(j, pix, mx, my);
                            if (mcu_idx + 1 < total_mcus) {
                                kjpg_check_restart(&br, j->dri, &mcus_left, dc_pred, &eobrun);
                            }
                        }
                    }
                }
                rendered_direct = 1;
            } else {
                for (int c = 0; c < j->ncomp; c++) {
                    if (!j->comp[c].coeffs) {
                        size_t bytes = (size_t)j->comp[c].bw * (size_t)j->comp[c].bh * 64u * sizeof(int16_t);
                        unsigned int pg = (unsigned int)((bytes + 4095u) / 4096u);
                        if (pg > pmm_get_free_pages()) { rc = -2; break; }
                        j->comp[c].coeffs = (int16_t*)pmm_alloc_pages(pg);
                        if (!j->comp[c].coeffs) { rc = -2; break; }
                        j->comp[c].coeff_pages = pg;
                        img_memset(j->comp[c].coeffs, 0, pg * 4096u);
                    }
                }
                if (rc != 0) { my_str_copy(err_msg, "Out of Memory decoding JPEG", err_msg_len); break; }

                if (ns == 1) {
                    int ci = scan_c[0];
                    kjpg_comp_t* c = &j->comp[ci];
                    int comp_w = (j->width  * c->h + j->max_h - 1) / j->max_h;
                    int comp_h = (j->height * c->v + j->max_v - 1) / j->max_v;
                    int bx_n = (comp_w + 7) / 8;
                    int by_n = (comp_h + 7) / 8;
                    if (bx_n > c->bw) bx_n = c->bw;
                    if (by_n > c->bh) by_n = c->bh;
                    size_t total_mcus = (size_t)bx_n * (size_t)by_n;
                    size_t mcu_idx = 0;
                    for (int by = 0; by < by_n && rc == 0; by++) {
                        for (int bx = 0; bx < bx_n && rc == 0; bx++, mcu_idx++) {
                            int16_t* blk = c->coeffs + ((size_t)by * (size_t)c->bw + (size_t)bx) * 64u;
                            if (kjpg_decode_scan_block(j, &br, blk, scan_td[0], scan_ta[0],
                                                       &dc_pred[0], &eobrun,
                                                       j->progressive, ss, se, ah, al) != 0) {
                                my_str_copy(err_msg, "JPEG scan error", err_msg_len);
                                rc = -1;
                                break;
                            }
                            if (mcu_idx + 1 < total_mcus) {
                                kjpg_check_restart(&br, j->dri, &mcus_left, dc_pred, &eobrun);
                            }
                        }
                    }
                } else {
                    size_t total_mcus = (size_t)j->mcu_cols * (size_t)j->mcu_rows;
                    size_t mcu_idx = 0;
                    for (int my = 0; my < j->mcu_rows && rc == 0; my++) {
                        for (int mx = 0; mx < j->mcu_cols && rc == 0; mx++, mcu_idx++) {
                            for (int k = 0; k < ns && rc == 0; k++) {
                                int ci = scan_c[k];
                                kjpg_comp_t* c = &j->comp[ci];
                                for (int v = 0; v < c->v && rc == 0; v++) {
                                    for (int u = 0; u < c->h && rc == 0; u++) {
                                        size_t bx = (size_t)(mx * c->h + u);
                                        size_t by = (size_t)(my * c->v + v);
                                        int16_t* blk = c->coeffs + (by * (size_t)c->bw + bx) * 64u;
                                        if (kjpg_decode_scan_block(j, &br, blk, scan_td[k], scan_ta[k],
                                                                   &dc_pred[k], &eobrun,
                                                                   j->progressive, ss, se, ah, al) != 0) {
                                            my_str_copy(err_msg, "JPEG scan error", err_msg_len);
                                            rc = -1;
                                            break;
                                        }
                                    }
                                }
                            }
                            if (rc == 0 && mcu_idx + 1 < total_mcus) {
                                kjpg_check_restart(&br, j->dri, &mcus_left, dc_pred, &eobrun);
                            }
                        }
                    }
                }
            }
            if (rc != 0) break;
            pos = br.pos;
            next_m = br.marker;
            if (!j->progressive && ns == j->ncomp) break;
        }
    }

    if (rc == 0 && (j->width <= 0 || j->height <= 0 || j->ncomp <= 0)) {
        my_str_copy(err_msg, "Missing JPEG frame", err_msg_len);
        rc = -1;
    }

    if (rc == 0 && !rendered_direct) {
        for (int c = 0; c < j->ncomp; c++) {
            if (!j->comp[c].coeffs || !j->qvalid[j->comp[c].qsel]) {
                my_str_copy(err_msg, "Incomplete JPEG scan", err_msg_len);
                rc = -1;
                break;
            }
        }
        if (rc == 0) {
            uint64_t npix = (uint64_t)j->width * (uint64_t)j->height * 4ull;
            out_pages = (unsigned int)((npix + 4095ull) / 4096ull);
            if (out_pages > pmm_get_free_pages()) {
                my_str_copy(err_msg, "Out of Memory for image", err_msg_len);
                rc = -2;
            } else {
                pix = (unsigned int*)pmm_alloc_pages(out_pages);
                if (!pix) {
                    my_str_copy(err_msg, "Out of Memory for image", err_msg_len);
                    rc = -2;
                }
            }
        }
        if (rc == 0) {
            for (int my = 0; my < j->mcu_rows; my++) {
                for (int mx = 0; mx < j->mcu_cols; mx++) {
                    for (int c = 0; c < j->ncomp; c++) {
                        kjpg_comp_t* cp = &j->comp[c];
                        const uint16_t* qt = j->qtab[cp->qsel];
                        for (int v = 0; v < cp->v; v++) {
                            for (int u = 0; u < cp->h; u++) {
                                size_t bx = (size_t)(mx * cp->h + u);
                                size_t by = (size_t)(my * cp->v + v);
                                const int16_t* zz = cp->coeffs + (by * (size_t)cp->bw + bx) * 64u;
                                int32_t blk32[64];
                                for (int i = 0; i < 64; i++) {
                                    blk32[k_kjpg_zz[i]] = (int32_t)zz[i] * (int32_t)qt[i];
                                }
                                kjpg_idct_8x8(blk32, j->mcu_px[c][v * cp->h + u]);
                            }
                        }
                    }
                    kjpg_write_mcu(j, pix, mx, my);
                }
            }
        }
    }

    for (int c = 0; c < 4; c++) {
        if (j->comp[c].coeffs && j->comp[c].coeff_pages > 0) {
            pmm_free_pages(j->comp[c].coeffs, j->comp[c].coeff_pages);
        }
    }

    if (rc == 0 && pix) {
        out_img->width = j->width;
        out_img->height = j->height;
        out_img->pixels = pix;
        out_img->page_count = out_pages;
        pmm_free_pages(j, ctx_pages);
        return 0;
    }

    if (pix && out_pages > 0) pmm_free_pages(pix, out_pages);
    pmm_free_pages(j, ctx_pages);
    return (rc == -2) ? -2 : -1;
}

// -------------------------------------------------------------
// Universal Image Loader API
// -------------------------------------------------------------
int image_load_from_memory(const unsigned char* data, unsigned int size, image_t* out_img, char* err_msg, int err_msg_len) {
    if (!data || size < 4 || !out_img) {
        my_str_copy(err_msg, "Invalid buffer", err_msg_len);
        return -1;
    }

    image_init(out_img);

    if (data[0] == 'B' && data[1] == 'M') {
        return decode_bmp(data, size, out_img, err_msg, err_msg_len);
    } else if (size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        return decode_png(data, size, out_img, err_msg, err_msg_len);
    } else if (data[0] == 0xFFu && data[1] == 0xD8u) {
        return decode_jpg(data, size, out_img, err_msg, err_msg_len);
    }

    my_str_copy(err_msg, "Unsupported format (BMP, PNG, and JPG supported)", err_msg_len);
    return -1;
}

int image_load_from_file(const char* path, image_t* out_img, char* err_msg, int err_msg_len) {
    if (!path || !path[0] || !out_img) {
        my_str_copy(err_msg, "Invalid file path", err_msg_len);
        return -1;
    }

    vfs_dirent_t dirent;
    if (vfs_find_file(path, &dirent) != VFS_OK) {
        my_str_copy(err_msg, "File not found", err_msg_len);
        return -1;
    }

    unsigned int file_size = dirent.size;
    if (file_size == 0) {
        my_str_copy(err_msg, "File is empty", err_msg_len);
        return -1;
    }

    unsigned int pages = (file_size + 4095) / 4096;
    if (pages > pmm_get_free_pages()) {
        unsigned int need_mb = (pages * 4) / 1024;
        unsigned int free_mb = (pmm_get_free_pages() * 4) / 1024;
        char nbuf[16], fbuf[16];
        my_int_to_str(need_mb, nbuf);
        my_int_to_str(free_mb, fbuf);
        err_msg[0] = '\0';
        my_str_copy(err_msg, "OOM: File needs ", err_msg_len);
        my_str_cat(err_msg, nbuf, err_msg_len);
        my_str_cat(err_msg, "MB (Free: ", err_msg_len);
        my_str_cat(err_msg, fbuf, err_msg_len);
        my_str_cat(err_msg, "MB)", err_msg_len);
        return -2;
    }

    unsigned char* file_buf = (unsigned char*)pmm_alloc_pages(pages);
    if (!file_buf) {
        my_str_copy(err_msg, "Out of Memory: Cannot allocate file buffer", err_msg_len);
        return -2;
    }

    int bytes_read = vfs_read_file(path, file_buf, file_size);
    if (bytes_read < (int)file_size) {
        pmm_free_pages(file_buf, pages);
        my_str_copy(err_msg, "Read error from VFS", err_msg_len);
        return -1;
    }

    int ret = image_load_from_memory(file_buf, file_size, out_img, err_msg, err_msg_len);
    pmm_free_pages(file_buf, pages);

    return ret;
}
