#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/fs.h>
#include <pxe/mem.h>
#include <pxe/str.h>

typedef struct {
    int       width;
    int       height;
    uint32_t* pixels; /* 0xAARRGGBB */
} viewer_img_t;

typedef struct {
    uint16_t counts[16];
    uint16_t symbols[288];
} tinf_tree_t;

typedef struct {
    const uint8_t* source;
    const uint8_t* source_end;
    uint32_t       tag;
    int            bitcount;
    tinf_tree_t    ltree;
    tinf_tree_t    dtree;
} tinf_data_t;

static uint16_t read_le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t read_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void tinf_build_tree(tinf_tree_t* t, const uint8_t* lengths, uint32_t num) {
    uint16_t offs[16];
    for (uint32_t i = 0; i < 16; ++i) t->counts[i] = 0;
    for (uint32_t i = 0; i < num; ++i) t->counts[lengths[i]]++;
    t->counts[0] = 0;

    uint32_t sum = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        offs[i] = (uint16_t)sum;
        sum += t->counts[i];
    }
    for (uint32_t i = 0; i < num; ++i) {
        if (lengths[i]) {
            t->symbols[offs[lengths[i]]++] = (uint16_t)i;
        }
    }
}

static int tinf_getbits(tinf_data_t* d, int num, uint32_t* out) {
    while (d->bitcount < num) {
        if (d->source >= d->source_end) return -1;
        d->tag |= ((uint32_t)(*d->source++)) << d->bitcount;
        d->bitcount += 8;
    }
    *out = d->tag & ((1u << num) - 1u);
    d->tag >>= num;
    d->bitcount -= num;
    return 0;
}

static int tinf_decode_symbol(tinf_data_t* d, const tinf_tree_t* t) {
    int base = 0, offs = 0;
    for (int len = 1; len <= 15; ++len) {
        uint32_t bit = 0;
        if (tinf_getbits(d, 1, &bit) < 0) return -1;
        base = (base << 1) | (int)bit;
        int count = t->counts[len];
        if (base < count) return t->symbols[offs + base];
        base -= count;
        offs += count;
    }
    return -1;
}

static const uint16_t k_len_base[30] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 0
};
static const uint8_t k_len_extra[30] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0, 0
};
static const uint16_t k_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t k_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const uint8_t k_cl_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static void tinf_build_fixed_trees(tinf_tree_t* lt, tinf_tree_t* dt) {
    uint8_t lengths[288];
    uint32_t i;
    for (i = 0; i < 144; ++i) lengths[i] = 8;
    for (; i < 256; ++i) lengths[i] = 9;
    for (; i < 280; ++i) lengths[i] = 7;
    for (; i < 288; ++i) lengths[i] = 8;
    tinf_build_tree(lt, lengths, 288);
    for (i = 0; i < 30; ++i) lengths[i] = 5;
    tinf_build_tree(dt, lengths, 30);
}

static int tinf_inflate_block(tinf_data_t* d, uint8_t* dest, uint32_t* destLen, uint32_t maxLen) {
    for (;;) {
        int sym = tinf_decode_symbol(d, &d->ltree);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (*destLen >= maxLen) return -1;
            dest[(*destLen)++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            uint32_t extra = 0;
            if (k_len_extra[sym] && tinf_getbits(d, k_len_extra[sym], &extra) < 0) return -1;
            uint32_t len = k_len_base[sym] + extra;
            int dsym = tinf_decode_symbol(d, &d->dtree);
            if (dsym < 0 || dsym >= 30) return -1;
            uint32_t dextra = 0;
            if (k_dist_extra[dsym] && tinf_getbits(d, k_dist_extra[dsym], &dextra) < 0) return -1;
            uint32_t dist = k_dist_base[dsym] + dextra;
            if (dist == 0 || *destLen < dist || *destLen + len > maxLen) return -1;
            const uint8_t* src = dest + *destLen - dist;
            while (len--) dest[(*destLen)++] = *src++;
        }
    }
}

static int tinf_uncompress(uint8_t* dest, uint32_t* destLen,
                           const uint8_t* src, uint32_t srcLen, uint32_t maxDestLen) {
    tinf_data_t d;
    memset(&d, 0, sizeof(d));
    d.source = src;
    d.source_end = src + srcLen;
    *destLen = 0;

    uint32_t bfinal = 0;
    do {
        uint32_t btype = 0;
        if (tinf_getbits(&d, 1, &bfinal) < 0) return -1;
        if (tinf_getbits(&d, 2, &btype) < 0) return -1;

        if (btype == 0) {
            int rem = d.bitcount & 7;
            d.tag >>= rem;
            d.bitcount -= rem;
            uint32_t len = 0, nlen = 0;
            if (tinf_getbits(&d, 16, &len) < 0) return -1;
            if (tinf_getbits(&d, 16, &nlen) < 0) return -1;
            if ((len ^ 0xFFFFu) != nlen) return -1;
            if (*destLen + len > maxDestLen) return -1;
            while (len--) {
                uint32_t b = 0;
                if (tinf_getbits(&d, 8, &b) < 0) return -1;
                dest[(*destLen)++] = (uint8_t)b;
            }
        } else if (btype == 1) {
            tinf_build_fixed_trees(&d.ltree, &d.dtree);
            if (tinf_inflate_block(&d, dest, destLen, maxDestLen) < 0) return -1;
        } else if (btype == 2) {
            uint32_t v = 0;
            if (tinf_getbits(&d, 5, &v) < 0) return -1;
            int hlit = (int)v + 257;
            if (tinf_getbits(&d, 5, &v) < 0) return -1;
            int hdist = (int)v + 1;
            if (tinf_getbits(&d, 4, &v) < 0) return -1;
            int hclen = (int)v + 4;
            if (hlit > 288 || hdist > 32 || hclen > 19) return -1;

            uint8_t cl_lengths[19];
            memset(cl_lengths, 0, sizeof(cl_lengths));
            for (int i = 0; i < hclen; i++) {
                if (tinf_getbits(&d, 3, &v) < 0) return -1;
                cl_lengths[k_cl_order[i]] = (uint8_t)v;
            }
            tinf_tree_t cl_tree;
            tinf_build_tree(&cl_tree, cl_lengths, 19);

            uint8_t lengths[288 + 32];
            memset(lengths, 0, sizeof(lengths));
            int num_codes = hlit + hdist;
            int idx = 0;
            while (idx < num_codes) {
                int sym = tinf_decode_symbol(&d, &cl_tree);
                if (sym < 0) return -1;
                if (sym < 16) {
                    lengths[idx++] = (uint8_t)sym;
                } else if (sym == 16) {
                    if (idx == 0) return -1;
                    if (tinf_getbits(&d, 2, &v) < 0) return -1;
                    int rep = (int)v + 3;
                    uint8_t prev = lengths[idx - 1];
                    while (rep-- && idx < num_codes) lengths[idx++] = prev;
                } else if (sym == 17) {
                    if (tinf_getbits(&d, 3, &v) < 0) return -1;
                    int rep = (int)v + 3;
                    while (rep-- && idx < num_codes) lengths[idx++] = 0;
                } else if (sym == 18) {
                    if (tinf_getbits(&d, 7, &v) < 0) return -1;
                    int rep = (int)v + 11;
                    while (rep-- && idx < num_codes) lengths[idx++] = 0;
                }
            }
            tinf_build_tree(&d.ltree, lengths, (uint32_t)hlit);
            tinf_build_tree(&d.dtree, lengths + hlit, (uint32_t)hdist);
            if (tinf_inflate_block(&d, dest, destLen, maxDestLen) < 0) return -1;
        } else {
            return -1;
        }
    } while (!bfinal);

    return 0;
}

static void img_free(viewer_img_t* img) {
    if (img->pixels) {
        free(img->pixels);
        img->pixels = NULL;
    }
    img->width = 0;
    img->height = 0;
}

static int decode_bmp(const uint8_t* data, uint32_t size, viewer_img_t* out,
                      char* err, size_t err_cap) {
    if (size < 54 || data[0] != 'B' || data[1] != 'M') {
        snprintf(err, err_cap, "Invalid BMP header");
        return -1;
    }
    uint32_t dib_size = read_le32(data + 14);
    uint32_t bf_off   = read_le32(data + 10);
    int32_t  bi_w     = (int32_t)read_le32(data + 18);
    int32_t  bi_h     = (int32_t)read_le32(data + 22);
    uint16_t bpp      = read_le16(data + 28);
    uint32_t comp     = read_le32(data + 30);

    if (bi_w <= 0 || bi_h == 0 || (comp != 0 && comp != 3) ||
        (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)) {
        snprintf(err, err_cap, "Unsupported BMP format");
        return -1;
    }

    int top_down = 0;
    int height = bi_h;
    if (height < 0) {
        top_down = 1;
        height = -height;
    }
    int width = bi_w;
    if (width > 4096 || height > 4096) {
        snprintf(err, err_cap, "Image dimensions too large");
        return -1;
    }

    uint32_t row_bytes = (((uint32_t)width * bpp + 31u) / 32u) * 4u;
    if (bf_off >= size || (uint64_t)bf_off + (uint64_t)row_bytes * (uint64_t)height > size) {
        snprintf(err, err_cap, "Truncated BMP pixel data");
        return -1;
    }

    uint32_t* pix = (uint32_t*)malloc((size_t)width * (size_t)height * 4u);
    if (!pix) {
        snprintf(err, err_cap, "Out of Memory for BMP pixels");
        return -1;
    }

    uint32_t pal[256];
    memset(pal, 0, sizeof(pal));
    if (bpp <= 8) {
        uint32_t pal_off = 14u + dib_size;
        int colors = 1 << bpp;
        if (pal_off + (uint32_t)colors * 4u <= size) {
            const uint8_t* p = data + pal_off;
            for (int i = 0; i < colors; i++) {
                pal[i] = 0xFF000000u | ((uint32_t)p[i * 4 + 2] << 16) |
                         ((uint32_t)p[i * 4 + 1] << 8) | p[i * 4 + 0];
            }
        }
    }

    const uint8_t* src_bits = data + bf_off;
    int has_alpha = 0;
    if (bpp == 32) {
        for (int y = 0; y < height && !has_alpha; y++) {
            const uint8_t* row = src_bits + (size_t)y * row_bytes;
            for (int x = 0; x < width; x++) {
                if (row[x * 4 + 3] != 0) {
                    has_alpha = 1;
                    break;
                }
            }
        }
    }

    for (int y = 0; y < height; y++) {
        int sy = top_down ? y : (height - 1 - y);
        const uint8_t* row = src_bits + (size_t)sy * row_bytes;
        uint32_t* dst = pix + (size_t)y * (size_t)width;
        if (bpp == 32) {
            for (int x = 0; x < width; x++) {
                uint8_t a = has_alpha ? row[x * 4 + 3] : 0xFFu;
                dst[x] = ((uint32_t)a << 24) | ((uint32_t)row[x * 4 + 2] << 16) |
                         ((uint32_t)row[x * 4 + 1] << 8) | row[x * 4 + 0];
            }
        } else if (bpp == 24) {
            for (int x = 0; x < width; x++) {
                dst[x] = 0xFF000000u | ((uint32_t)row[x * 3 + 2] << 16) |
                         ((uint32_t)row[x * 3 + 1] << 8) | row[x * 3 + 0];
            }
        } else if (bpp == 16) {
            for (int x = 0; x < width; x++) {
                uint16_t c = read_le16(row + x * 2);
                uint8_t r = (uint8_t)((c >> 10) & 0x1Fu); r = (uint8_t)((r << 3) | (r >> 2));
                uint8_t g = (uint8_t)((c >> 5) & 0x1Fu);  g = (uint8_t)((g << 3) | (g >> 2));
                uint8_t b = (uint8_t)(c & 0x1Fu);         b = (uint8_t)((b << 3) | (b >> 2));
                dst[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            }
        } else if (bpp == 8) {
            for (int x = 0; x < width; x++) dst[x] = pal[row[x]];
        } else if (bpp == 4) {
            for (int x = 0; x < width; x++) {
                uint8_t byte = row[x >> 1];
                uint8_t idx = (x & 1) ? (byte & 0x0Fu) : (byte >> 4);
                dst[x] = pal[idx];
            }
        } else {
            for (int x = 0; x < width; x++) {
                uint8_t byte = row[x >> 3];
                uint8_t idx = (byte >> (7 - (x & 7))) & 1u;
                dst[x] = pal[idx];
            }
        }
    }

    out->width = width;
    out->height = height;
    out->pixels = pix;
    return 0;
}

static int paeth_predict(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static int decode_png(const uint8_t* data, uint32_t size, viewer_img_t* out,
                      char* err, size_t err_cap) {
    if (size < 33 ||
        data[0] != 0x89 || data[1] != 'P' || data[2] != 'N' || data[3] != 'G' ||
        data[4] != 0x0D || data[5] != 0x0A || data[6] != 0x1A || data[7] != 0x0A) {
        snprintf(err, err_cap, "Invalid PNG signature");
        return -1;
    }

    int width = 0, height = 0, bit_depth = 0, color_type = 0, interlace = 0;
    uint8_t pal[256 * 4];
    memset(pal, 0xFF, sizeof(pal));
    int has_pal = 0;

    uint32_t pos = 8;
    uint32_t idat_total = 0;
    while (pos + 12 <= size) {
        uint32_t clen = read_be32(data + pos);
        uint32_t ctyp = read_be32(data + pos + 4);
        if (clen > size - pos - 12) break;
        const uint8_t* cd = data + pos + 8;

        if (ctyp == 0x49484452u && clen >= 13) { /* IHDR */
            width      = (int)read_be32(cd);
            height     = (int)read_be32(cd + 4);
            bit_depth  = cd[8];
            color_type = cd[9];
            interlace  = cd[12];
        } else if (ctyp == 0x504C5445u) { /* PLTE */
            uint32_t entries = clen / 3u;
            if (entries > 256u) entries = 256u;
            for (uint32_t i = 0; i < entries; i++) {
                pal[i * 4 + 0] = cd[i * 3 + 0];
                pal[i * 4 + 1] = cd[i * 3 + 1];
                pal[i * 4 + 2] = cd[i * 3 + 2];
                pal[i * 4 + 3] = 0xFFu;
            }
            has_pal = 1;
        } else if (ctyp == 0x74524E53u) { /* tRNS */
            if (color_type == 3 && has_pal) {
                uint32_t entries = (clen < 256u) ? clen : 256u;
                for (uint32_t i = 0; i < entries; i++) pal[i * 4 + 3] = cd[i];
            }
        } else if (ctyp == 0x49444154u) { /* IDAT */
            idat_total += clen;
        } else if (ctyp == 0x49454E44u) { /* IEND */
            break;
        }
        pos += 12u + clen;
    }

    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        bit_depth != 8 || interlace != 0 || idat_total < 4) {
        snprintf(err, err_cap, "Unsupported PNG format (need 8-bit non-interlaced)");
        return -1;
    }

    int bpp = 4;
    if (color_type == 0 || color_type == 3) bpp = 1;
    else if (color_type == 2) bpp = 3;
    else if (color_type == 4) bpp = 2;
    else if (color_type == 6) bpp = 4;
    else {
        snprintf(err, err_cap, "Unsupported PNG color type");
        return -1;
    }

    uint32_t stride = 1u + (uint32_t)width * (uint32_t)bpp;
    uint32_t raw_size = stride * (uint32_t)height;

    uint8_t* idat_buf = (uint8_t*)malloc(idat_total);
    uint8_t* raw_buf  = (uint8_t*)malloc(raw_size);
    if (!idat_buf || !raw_buf) {
        free(idat_buf);
        free(raw_buf);
        snprintf(err, err_cap, "Out of Memory decoding PNG");
        return -1;
    }

    pos = 8;
    uint32_t cpos = 0;
    while (pos + 12 <= size) {
        uint32_t clen = read_be32(data + pos);
        uint32_t ctyp = read_be32(data + pos + 4);
        if (clen > size - pos - 12) break;
        if (ctyp == 0x49444154u) {
            memcpy(idat_buf + cpos, data + pos + 8, clen);
            cpos += clen;
        } else if (ctyp == 0x49454E44u) {
            break;
        }
        pos += 12u + clen;
    }

    uint32_t decomp_len = 0;
    int inf_rc = tinf_uncompress(raw_buf, &decomp_len, idat_buf + 2, idat_total - 2, raw_size);
    free(idat_buf);

    if (inf_rc != 0 || decomp_len < raw_size) {
        free(raw_buf);
        snprintf(err, err_cap, "PNG inflate failed");
        return -1;
    }

    uint32_t* pix = (uint32_t*)malloc((size_t)width * (size_t)height * 4u);
    if (!pix) {
        free(raw_buf);
        snprintf(err, err_cap, "Out of Memory for PNG image");
        return -1;
    }

    const uint8_t* prev_row = NULL;
    for (int y = 0; y < height; y++) {
        uint8_t* row = raw_buf + (size_t)y * stride;
        int ftype = row[0];
        uint8_t* cur = row + 1;
        int row_bytes = width * bpp;

        if (ftype == 1) {
            for (int i = bpp; i < row_bytes; i++) cur[i] += cur[i - bpp];
        } else if (ftype == 2) {
            if (prev_row) {
                for (int i = 0; i < row_bytes; i++) cur[i] += prev_row[i];
            }
        } else if (ftype == 3) {
            for (int i = 0; i < row_bytes; i++) {
                int a = (i >= bpp) ? cur[i - bpp] : 0;
                int b = prev_row ? prev_row[i] : 0;
                cur[i] += (uint8_t)((a + b) >> 1);
            }
        } else if (ftype == 4) {
            for (int i = 0; i < row_bytes; i++) {
                int a = (i >= bpp) ? cur[i - bpp] : 0;
                int b = prev_row ? prev_row[i] : 0;
                int c = (prev_row && i >= bpp) ? prev_row[i - bpp] : 0;
                cur[i] += (uint8_t)paeth_predict(a, b, c);
            }
        }
        prev_row = cur;

        uint32_t* dst = pix + (size_t)y * (size_t)width;
        if (color_type == 6) {
            for (int x = 0; x < width; x++) {
                dst[x] = ((uint32_t)cur[x * 4 + 3] << 24) |
                         ((uint32_t)cur[x * 4 + 0] << 16) |
                         ((uint32_t)cur[x * 4 + 1] << 8)  | cur[x * 4 + 2];
            }
        } else if (color_type == 2) {
            for (int x = 0; x < width; x++) {
                dst[x] = 0xFF000000u |
                         ((uint32_t)cur[x * 3 + 0] << 16) |
                         ((uint32_t)cur[x * 3 + 1] << 8)  | cur[x * 3 + 2];
            }
        } else if (color_type == 3) {
            for (int x = 0; x < width; x++) {
                uint8_t idx = cur[x];
                dst[x] = ((uint32_t)pal[idx * 4 + 3] << 24) |
                         ((uint32_t)pal[idx * 4 + 0] << 16) |
                         ((uint32_t)pal[idx * 4 + 1] << 8)  | pal[idx * 4 + 2];
            }
        } else if (color_type == 0) {
            for (int x = 0; x < width; x++) {
                uint8_t v = cur[x];
                dst[x] = 0xFF000000u | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
            }
        } else {
            for (int x = 0; x < width; x++) {
                uint8_t v = cur[x * 2 + 0];
                uint8_t a = cur[x * 2 + 1];
                dst[x] = ((uint32_t)a << 24) | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
            }
        }
    }

    free(raw_buf);
    out->width = width;
    out->height = height;
    out->pixels = pix;
    return 0;
}

static const int k_zoom_steps[] = { 12, 25, 33, 50, 75, 100, 150, 200, 300, 400 };
#define NUM_ZOOM_STEPS ((int)(sizeof(k_zoom_steps) / sizeof(k_zoom_steps[0])))

typedef struct {
    int          win_id;
    viewer_img_t img;
    char         img_path[256];
    int          zoom_pct; /* 0 = Fit */
    int          pan_x;
    int          pan_y;
    int          panning;
    int          last_mx;
    int          last_my;

    int          wp_menu_open;
    int          open_modal;
    char         open_input[256];
    int          open_len;
    int          err_modal;
    char         err_msg[96];
} viewer_t;

static void make_sample_image(viewer_t* vw) {
    img_free(&vw->img);
    int w = 320, h = 240;
    uint32_t* pix = (uint32_t*)malloc((size_t)w * (size_t)h * 4u);
    if (!pix) return;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t r = (uint8_t)((x * 255) / w);
            uint8_t g = (uint8_t)((y * 255) / h);
            uint8_t b = (uint8_t)(((w - x) * 200) / w + ((h - y) * 55) / h);
            if (((x / 24) ^ (y / 24)) & 1) {
                r = (uint8_t)((r >> 1) + 64);
                g = (uint8_t)((g >> 1) + 64);
                b = (uint8_t)((b >> 1) + 64);
            }
            pix[y * w + x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
    vw->img.width = w;
    vw->img.height = h;
    vw->img.pixels = pix;
    vw->img_path[0] = '\0';
    vw->zoom_pct = 0;
    vw->pan_x = 0;
    vw->pan_y = 0;
    pxe_win_set_title(vw->win_id, "Image Viewer - Sample Image");
}

static void viewer_open_file(viewer_t* vw, const char* path) {
    if (!path || !path[0]) return;
    img_free(&vw->img);
    vw->zoom_pct = 0;
    vw->pan_x = 0;
    vw->pan_y = 0;

    pxe_dirent_t st;
    if (pxe_stat(path, &st) != 0 || st.is_dir || st.size < 8) {
        vw->err_modal = 1;
        snprintf(vw->err_msg, sizeof(vw->err_msg), "File not found or unreadable.");
        pxe_beep();
        return;
    }
    if (st.size > 16u * 1024u * 1024u) {
        vw->err_modal = 1;
        snprintf(vw->err_msg, sizeof(vw->err_msg), "File too large (>16 MB).");
        pxe_beep();
        return;
    }

    int fd = pxe_open(path);
    if (fd < 0) {
        vw->err_modal = 1;
        snprintf(vw->err_msg, sizeof(vw->err_msg), "Failed to open file.");
        pxe_beep();
        return;
    }

    uint8_t* raw = (uint8_t*)malloc((size_t)st.size);
    if (!raw) {
        pxe_close(fd);
        vw->err_modal = 1;
        snprintf(vw->err_msg, sizeof(vw->err_msg), "Out of Memory reading file.");
        pxe_beep();
        return;
    }

    uint64_t got = 0;
    while (got < st.size) {
        size_t chunk = (size_t)(st.size - got);
        if (chunk > 32768u) chunk = 32768u;
        int r = pxe_read(fd, raw + got, chunk);
        if (r <= 0) break;
        got += (uint64_t)r;
    }
    pxe_close(fd);

    char err[96];
    err[0] = '\0';
    int rc = -1;
    if (got >= 8 && raw[0] == 'B' && raw[1] == 'M') {
        rc = decode_bmp(raw, (uint32_t)got, &vw->img, err, sizeof(err));
    } else if (got >= 8 && raw[0] == 0x89 && raw[1] == 'P' && raw[2] == 'N' && raw[3] == 'G') {
        rc = decode_png(raw, (uint32_t)got, &vw->img, err, sizeof(err));
    } else {
        snprintf(err, sizeof(err), "Unsupported format (BMP/PNG only).");
    }
    free(raw);

    if (rc != 0) {
        vw->err_modal = 1;
        snprintf(vw->err_msg, sizeof(vw->err_msg), "%s", err[0] ? err : "Image decode error.");
        pxe_beep();
        pxe_win_set_title(vw->win_id, "Image Viewer - Error");
        return;
    }

    strncpy(vw->img_path, path, sizeof(vw->img_path) - 1);
    vw->img_path[sizeof(vw->img_path) - 1] = '\0';

    const char* bn = path;
    for (const char* p = path; *p; p++) {
        if (*p == '\\' || *p == '/') bn = p + 1;
    }
    char title[48];
    snprintf(title, sizeof(title), "Viewer - %s", bn);
    pxe_win_set_title(vw->win_id, title);
}

static uint32_t blend_bg_rgb32(uint32_t argb) {
    uint32_t a = (argb >> 24) & 0xFFu;
    if (a == 255u) return argb & 0x00FFFFFFu;
    if (a == 0u)   return 0x00808080u;
    uint32_t inv = 255u - a;
    uint32_t r = ((((argb >> 16) & 0xFFu) * a) + 128u * inv) / 255u;
    uint32_t g = ((((argb >> 8) & 0xFFu) * a) + 128u * inv) / 255u;
    uint32_t b = (((argb & 0xFFu) * a) + 128u * inv) / 255u;
    return (r << 16) | (g << 8) | b;
}

static void render_viewer(const pxe_win_surface_t* surf, viewer_t* vw) {
    /* Toolbar (y=0..25) */
    pxe_fill_rect(surf, 0, 0, surf->width, 26, PXE_COL_LIGHT_GRAY);
    pxe_draw_hline(surf, 0, 25, surf->width, PXE_COL_DARK_GRAY);
    pxe_draw_button(surf, 6,   3, 46, 20, "Open", 0, 1);
    pxe_draw_button(surf, 56,  3, 24, 20, "-", 0, 1);
    pxe_draw_button(surf, 84,  3, 24, 20, "+", 0, 1);
    pxe_draw_button(surf, 112, 3, 38, 20, "1:1", 0, 1);
    pxe_draw_button(surf, 154, 3, 38, 20, "Fit", 0, 1);
    pxe_draw_button(surf, 198, 3, 114, 20, "Set Wallpaper", 0, vw->img.pixels != NULL);

    /* Status bar */
    int sb_y = surf->height - 20;
    pxe_fill_rect(surf, 0, sb_y, surf->width, 20, PXE_COL_LIGHT_GRAY);
    pxe_draw_hline(surf, 0, sb_y, surf->width, PXE_COL_DARK_GRAY);
    char status[128];
    if (vw->img.pixels && vw->img.width > 0) {
        uint32_t kb = ((uint32_t)vw->img.width * (uint32_t)vw->img.height * 4u) / 1024u;
        if (vw->zoom_pct == 0) {
            snprintf(status, sizeof(status), "%dx%d 32-bit | Zoom: Fit | %u KB",
                     vw->img.width, vw->img.height, (unsigned)kb);
        } else {
            snprintf(status, sizeof(status), "%dx%d 32-bit | Zoom: %d%% | %u KB",
                     vw->img.width, vw->img.height, vw->zoom_pct, (unsigned)kb);
        }
    } else {
        snprintf(status, sizeof(status), "No image loaded");
    }
    pxe_draw_text_small(surf, 8, sb_y + 7, status, PXE_COL_BLACK);

    /* Viewport */
    int vp_x = 0, vp_y = 26, vp_w = surf->width, vp_h = sb_y - vp_y;
    if (vp_w <= 0 || vp_h <= 0) return;
    pxe_fill_rect(surf, vp_x, vp_y, vp_w, vp_h, PXE_COL_DARK_GRAY);

    if (!vw->img.pixels || vw->img.width <= 0 || vw->img.height <= 0) {
        pxe_draw_text(surf, vp_x + vp_w / 2 - 44, vp_y + vp_h / 2 - 8,
                      "(No Image)", PXE_COL_WHITE);
    } else {
        int dw, dh;
        if (vw->zoom_pct == 0) {
            if ((uint64_t)vw->img.width * (uint64_t)vp_h >
                (uint64_t)vp_w * (uint64_t)vw->img.height) {
                dw = vp_w;
                dh = (int)(((uint64_t)vw->img.height * (uint64_t)vp_w) / (uint64_t)vw->img.width);
            } else {
                dh = vp_h;
                dw = (int)(((uint64_t)vw->img.width * (uint64_t)vp_h) / (uint64_t)vw->img.height);
            }
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            vw->pan_x = (vp_w - dw) / 2;
            vw->pan_y = (vp_h - dh) / 2;
        } else {
            dw = (int)(((uint64_t)vw->img.width * (uint64_t)vw->zoom_pct) / 100u);
            dh = (int)(((uint64_t)vw->img.height * (uint64_t)vw->zoom_pct) / 100u);
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
        }

        int ix0 = vp_x + vw->pan_x;
        int iy0 = vp_y + vw->pan_y;
        int rx0 = (ix0 < vp_x) ? vp_x : ix0;
        int ry0 = (iy0 < vp_y) ? vp_y : iy0;
        int rx1 = (ix0 + dw > vp_x + vp_w) ? (vp_x + vp_w) : (ix0 + dw);
        int ry1 = (iy0 + dh > vp_y + vp_h) ? (vp_y + vp_h) : (iy0 + dh);

        for (int y = ry0; y < ry1; y++) {
            int sy = (int)(((uint64_t)(y - iy0) * (uint64_t)vw->img.height) / (uint64_t)dh);
            if (sy >= vw->img.height) sy = vw->img.height - 1;
            const uint32_t* srow = vw->img.pixels + (size_t)sy * (size_t)vw->img.width;
            if (surf->format == PXE_CANVAS_RGB32) {
                uint32_t* drow = (uint32_t*)((uint8_t*)surf->pixels + (size_t)y * (size_t)surf->pitch);
                for (int x = rx0; x < rx1; x++) {
                    int sx = (int)(((uint64_t)(x - ix0) * (uint64_t)vw->img.width) / (uint64_t)dw);
                    if (sx >= vw->img.width) sx = vw->img.width - 1;
                    drow[x] = blend_bg_rgb32(srow[sx]);
                }
            } else {
                for (int x = rx0; x < rx1; x++) {
                    int sx = (int)(((uint64_t)(x - ix0) * (uint64_t)vw->img.width) / (uint64_t)dw);
                    if (sx >= vw->img.width) sx = vw->img.width - 1;
                    pxe_put_rgb32(surf, x, y, blend_bg_rgb32(srow[sx]));
                }
            }
        }
    }

    if (vw->wp_menu_open) {
        int mx = 198, my = 25, mw = 114, mh = 4 * 18 + 4;
        pxe_draw_bevel_box(surf, mx, my, mw, mh, 0, PXE_COL_WHITE);
        pxe_draw_text_small(surf, mx + 8, my + 6 + 0 * 18, "1. Fill", PXE_COL_BLACK);
        pxe_draw_text_small(surf, mx + 8, my + 6 + 1 * 18, "2. Fit", PXE_COL_BLACK);
        pxe_draw_text_small(surf, mx + 8, my + 6 + 2 * 18, "3. Stretch", PXE_COL_BLACK);
        pxe_draw_text_small(surf, mx + 8, my + 6 + 3 * 18, "4. Tile", PXE_COL_BLACK);
    }

    if (vw->open_modal) {
        int dw = 280, dh = 94;
        if (dw > surf->width - 12) dw = surf->width - 12;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        pxe_draw_bevel_box(surf, dx, dy, dw, dh, 0, PXE_COL_LIGHT_GRAY);
        pxe_fill_rect(surf, dx + 2, dy + 2, dw - 4, 18, PXE_COL_DARK_BLUE);
        pxe_draw_text(surf, dx + 8, dy + 3, "Open Image (BMP / PNG)", PXE_COL_WHITE);
        pxe_draw_bevel_box(surf, dx + 10, dy + 28, dw - 20, 22, 1, PXE_COL_WHITE);
        int max_c = (dw - 28) / 6;
        int start = (vw->open_len > max_c) ? (vw->open_len - max_c) : 0;
        pxe_draw_text_small(surf, dx + 14, dy + 36, vw->open_input + start, PXE_COL_BLACK);
        pxe_draw_vline(surf, dx + 14 + (vw->open_len - start) * 6, dy + 32, 14, PXE_COL_BLACK);
        pxe_draw_button(surf, dx + dw / 2 - 72, dy + 60, 64, 22, "Open", 0, 1);
        pxe_draw_button(surf, dx + dw / 2 + 8,  dy + 60, 64, 22, "Cancel", 0, 1);
    } else if (vw->err_modal) {
        int dw = 300, dh = 90;
        if (dw > surf->width - 12) dw = surf->width - 12;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        pxe_draw_bevel_box(surf, dx, dy, dw, dh, 0, PXE_COL_LIGHT_GRAY);
        pxe_fill_rect(surf, dx + 12, dy + 12, 22, 22, PXE_COL_DARK_RED);
        pxe_draw_text(surf, dx + 19, dy + 15, "!", PXE_COL_WHITE);
        pxe_draw_text(surf, dx + 42, dy + 14, "Image Error", PXE_COL_DARK_RED);
        pxe_draw_text_small(surf, dx + 42, dy + 34, vw->err_msg, PXE_COL_BLACK);
        pxe_draw_button(surf, dx + dw / 2 - 30, dy + 58, 60, 22, "OK", 0, 1);
    }
}

static void zoom_in(viewer_t* vw) {
    if (vw->zoom_pct == 0) vw->zoom_pct = 50;
    for (int i = 0; i < NUM_ZOOM_STEPS; i++) {
        if (k_zoom_steps[i] > vw->zoom_pct) {
            vw->zoom_pct = k_zoom_steps[i];
            break;
        }
    }
}

static void zoom_out(viewer_t* vw) {
    if (vw->zoom_pct == 0) vw->zoom_pct = 100;
    for (int i = NUM_ZOOM_STEPS - 1; i >= 0; i--) {
        if (k_zoom_steps[i] < vw->zoom_pct) {
            vw->zoom_pct = k_zoom_steps[i];
            break;
        }
    }
}

int main(int argc, char** argv) {
    viewer_t* vw = (viewer_t*)calloc(1, sizeof(viewer_t));
    if (!vw) return 1;

    pxe_win_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.x = 72;
    desc.y = 42;
    desc.w = 480;
    desc.h = 340;
    desc.min_w = 320;
    desc.min_h = 220;
    desc.format = PXE_CANVAS_RGB32;
    desc.flags = PXE_WIN_RESIZABLE;
    strcpy(desc.title, "Image Viewer");

    pxe_win_surface_t surf;
    vw->win_id = pxe_win_create(&desc, &surf);
    if (vw->win_id < 0) {
        free(vw);
        return 1;
    }

    snprintf(vw->open_input, sizeof(vw->open_input), "C:\\");
    vw->open_len = 3;

    if (argc > 1 && argv[1] && argv[1][0]) {
        viewer_open_file(vw, argv[1]);
    } else {
        make_sample_image(vw);
    }

    render_viewer(&surf, vw);
    pxe_win_present(vw->win_id);

    int running = 1;
    while (running) {
        pxe_event_t ev;
        int rc = pxe_win_wait_event(vw->win_id, &ev);
        if (rc < 0) break;
        if (rc == 0) continue;

        int dirty = 0;
        switch (ev.type) {
            case PXE_EV_CLOSE:
                running = 0;
                break;
            case PXE_EV_RESIZE:
                if (pxe_win_get_surface(vw->win_id, &surf) == 0) dirty = 1;
                break;
            case PXE_EV_REDRAW:
                dirty = 1;
                break;
            case PXE_EV_ACTIVATE:
                if (ev.text[0]) viewer_open_file(vw, ev.text);
                dirty = 1;
                break;
            case PXE_EV_MOUSE_DOWN:
                if (ev.buttons & PXE_BTN_LEFT) {
                    int mx = ev.x, my = ev.y;
                    if (vw->err_modal) {
                        int dw = (surf.width - 12 < 300) ? surf.width - 12 : 300;
                        int dx = (surf.width - dw) / 2;
                        int dy = (surf.height - 90) / 2;
                        if (pxe_point_in_rect(mx, my, dx + dw / 2 - 30, dy + 58, 60, 22)) {
                            vw->err_modal = 0;
                            dirty = 1;
                        }
                        break;
                    }
                    if (vw->open_modal) {
                        int dw = (surf.width - 12 < 280) ? surf.width - 12 : 280;
                        int dx = (surf.width - dw) / 2;
                        int dy = (surf.height - 94) / 2;
                        if (pxe_point_in_rect(mx, my, dx + dw / 2 - 72, dy + 60, 64, 22)) {
                            vw->open_modal = 0;
                            if (vw->open_len > 0) viewer_open_file(vw, vw->open_input);
                            dirty = 1;
                        } else if (pxe_point_in_rect(mx, my, dx + dw / 2 + 8, dy + 60, 64, 22)) {
                            vw->open_modal = 0;
                            dirty = 1;
                        }
                        break;
                    }
                    if (vw->wp_menu_open) {
                        if (pxe_point_in_rect(mx, my, 198, 27, 114, 4 * 18)) {
                            int item = (my - 27) / 18;
                            if (item >= 0 && item < 4) {
                                if (vw->img_path[0]) {
                                    if (pxe_desktop_set_wallpaper(vw->img_path, item) == 0) {
                                        pxe_settings_save();
                                    } else {
                                        vw->err_modal = 1;
                                        snprintf(vw->err_msg, sizeof(vw->err_msg),
                                                 "Failed to apply wallpaper.");
                                        pxe_beep();
                                    }
                                } else {
                                    vw->err_modal = 1;
                                    snprintf(vw->err_msg, sizeof(vw->err_msg),
                                             "Open a BMP/PNG file first.");
                                    pxe_beep();
                                }
                            }
                        }
                        vw->wp_menu_open = 0;
                        dirty = 1;
                        break;
                    }

                    if (my >= 3 && my < 23) {
                        if (pxe_point_in_rect(mx, my, 6, 3, 46, 20)) {
                            vw->open_modal = 1;
                        } else if (pxe_point_in_rect(mx, my, 56, 3, 24, 20)) {
                            zoom_out(vw);
                        } else if (pxe_point_in_rect(mx, my, 84, 3, 24, 20)) {
                            zoom_in(vw);
                        } else if (pxe_point_in_rect(mx, my, 112, 3, 38, 20)) {
                            vw->zoom_pct = 100;
                            vw->pan_x = 0;
                            vw->pan_y = 0;
                        } else if (pxe_point_in_rect(mx, my, 154, 3, 38, 20)) {
                            vw->zoom_pct = 0;
                            vw->pan_x = 0;
                            vw->pan_y = 0;
                        } else if (pxe_point_in_rect(mx, my, 198, 3, 114, 20) && vw->img.pixels) {
                            vw->wp_menu_open = 1;
                        }
                        dirty = 1;
                        break;
                    }

                    if (my >= 26 && my < surf.height - 20) {
                        vw->panning = 1;
                        vw->last_mx = mx;
                        vw->last_my = my;
                    }
                }
                break;
            case PXE_EV_MOUSE_MOVE:
                if (vw->panning && (ev.buttons & PXE_BTN_LEFT)) {
                    int dx = ev.x - vw->last_mx;
                    int dy = ev.y - vw->last_my;
                    if (dx != 0 || dy != 0) {
                        if (vw->zoom_pct == 0) vw->zoom_pct = 100;
                        vw->pan_x += dx;
                        vw->pan_y += dy;
                        vw->last_mx = ev.x;
                        vw->last_my = ev.y;
                        dirty = 1;
                    }
                }
                break;
            case PXE_EV_MOUSE_UP:
                vw->panning = 0;
                break;
            case PXE_EV_KEY_DOWN:
                if (vw->err_modal) {
                    if (ev.key == PXE_KEY_ENTER || ev.key == PXE_KEY_ESC || ev.key == ' ') {
                        vw->err_modal = 0;
                        dirty = 1;
                    }
                    break;
                }
                if (vw->open_modal) {
                    if (ev.key == PXE_KEY_ESC) {
                        vw->open_modal = 0;
                        dirty = 1;
                    } else if (ev.key == PXE_KEY_ENTER) {
                        vw->open_modal = 0;
                        if (vw->open_len > 0) viewer_open_file(vw, vw->open_input);
                        dirty = 1;
                    } else if (ev.key == PXE_KEY_BACKSPACE) {
                        if (vw->open_len > 0) {
                            vw->open_input[--vw->open_len] = '\0';
                            dirty = 1;
                        }
                    } else if (ev.key >= 32 && ev.key <= 126 && vw->open_len < 250) {
                        vw->open_input[vw->open_len++] = (char)ev.key;
                        vw->open_input[vw->open_len] = '\0';
                        dirty = 1;
                    }
                    break;
                }

                if (ev.key == '+' || ev.key == '=') {
                    zoom_in(vw);
                    dirty = 1;
                } else if (ev.key == '-' || ev.key == '_') {
                    zoom_out(vw);
                    dirty = 1;
                } else if (ev.key == '0') {
                    vw->zoom_pct = 0;
                    vw->pan_x = 0;
                    vw->pan_y = 0;
                    dirty = 1;
                } else if (ev.key == '1') {
                    vw->zoom_pct = 100;
                    vw->pan_x = 0;
                    vw->pan_y = 0;
                    dirty = 1;
                } else if (ev.key == 'o' || ev.key == 'O') {
                    vw->open_modal = 1;
                    dirty = 1;
                } else if (ev.key == PXE_KEY_UP) {
                    if (vw->zoom_pct == 0) vw->zoom_pct = 100;
                    vw->pan_y += 24;
                    dirty = 1;
                } else if (ev.key == PXE_KEY_DOWN) {
                    if (vw->zoom_pct == 0) vw->zoom_pct = 100;
                    vw->pan_y -= 24;
                    dirty = 1;
                } else if (ev.key == PXE_KEY_LEFT) {
                    if (vw->zoom_pct == 0) vw->zoom_pct = 100;
                    vw->pan_x += 24;
                    dirty = 1;
                } else if (ev.key == PXE_KEY_RIGHT) {
                    if (vw->zoom_pct == 0) vw->zoom_pct = 100;
                    vw->pan_x -= 24;
                    dirty = 1;
                } else if (ev.key == PXE_KEY_ESC) {
                    if (vw->wp_menu_open) {
                        vw->wp_menu_open = 0;
                        dirty = 1;
                    } else {
                        running = 0;
                    }
                }
                break;
            default:
                break;
        }

        if (dirty && running) {
            render_viewer(&surf, vw);
            pxe_win_present(vw->win_id);
        }
    }

    img_free(&vw->img);
    pxe_win_destroy(vw->win_id);
    free(vw);
    return 0;
}
