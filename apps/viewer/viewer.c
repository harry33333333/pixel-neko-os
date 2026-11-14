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

static uint16_t read_be16(const uint8_t* p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
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

static int tinf_inflate_block(tinf_data_t* d, uint8_t* dest, size_t* destLen, size_t maxLen) {
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
            size_t len = (size_t)(k_len_base[sym] + extra);
            int dsym = tinf_decode_symbol(d, &d->dtree);
            if (dsym < 0 || dsym >= 30) return -1;
            uint32_t dextra = 0;
            if (k_dist_extra[dsym] && tinf_getbits(d, k_dist_extra[dsym], &dextra) < 0) return -1;
            size_t dist = (size_t)(k_dist_base[dsym] + dextra);
            if (dist == 0 || *destLen < dist || *destLen + len > maxLen) return -1;
            const uint8_t* src = dest + *destLen - dist;
            while (len--) dest[(*destLen)++] = *src++;
        }
    }
}

static int tinf_uncompress(uint8_t* dest, size_t* destLen,
                           const uint8_t* src, size_t srcLen, size_t maxDestLen) {
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
            if (*destLen + (size_t)len > maxDestLen) return -1;
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

static void bmp_decode_row(int bpp, int width, const uint8_t* row,
                           const uint32_t pal[256], int has_alpha, uint32_t* dst) {
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

static int decode_bmp(const uint8_t* data, size_t size, viewer_img_t* out,
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
    if (width > 8192 || height > 8192) {
        snprintf(err, err_cap, "Image exceeds 8K limit (8192x8192)");
        return -1;
    }

    uint64_t row_bytes = (((uint64_t)width * (uint64_t)bpp + 31ull) / 32ull) * 4ull;
    if ((uint64_t)bf_off >= (uint64_t)size ||
        (uint64_t)bf_off + row_bytes * (uint64_t)height > (uint64_t)size) {
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
        uint64_t pal_off = 14ull + (uint64_t)dib_size;
        int colors = 1 << bpp;
        if (pal_off + (uint64_t)colors * 4ull <= (uint64_t)size) {
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
            const uint8_t* row = src_bits + (size_t)y * (size_t)row_bytes;
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
        const uint8_t* row = src_bits + (size_t)sy * (size_t)row_bytes;
        bmp_decode_row(bpp, width, row, pal, has_alpha, pix + (size_t)y * (size_t)width);
    }

    out->width = width;
    out->height = height;
    out->pixels = pix;
    return 0;
}

static int stream_read_exact(int fd, uint8_t* io_buf, size_t io_cap,
                             size_t* io_pos, size_t* io_len,
                             uint8_t* dst, size_t want) {
    size_t got = 0;
    while (got < want) {
        if (*io_pos >= *io_len) {
            int n = pxe_read(fd, io_buf, io_cap);
            if (n <= 0) return -1;
            *io_pos = 0;
            *io_len = (size_t)n;
        }
        size_t avail = *io_len - *io_pos;
        size_t take = want - got;
        if (take > avail) take = avail;
        memcpy(dst + got, io_buf + *io_pos, take);
        *io_pos += take;
        got += take;
    }
    return 0;
}

static int decode_bmp_stream(int fd, uint64_t file_size, const uint8_t hdr[64],
                             viewer_img_t* out, char* err, size_t err_cap) {
    uint32_t dib_size = read_le32(hdr + 14);
    uint32_t bf_off   = read_le32(hdr + 10);
    int32_t  bi_w     = (int32_t)read_le32(hdr + 18);
    int32_t  bi_h     = (int32_t)read_le32(hdr + 22);
    uint16_t bpp      = read_le16(hdr + 28);
    uint32_t comp     = read_le32(hdr + 30);

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
    if (width > 8192 || height > 8192) {
        snprintf(err, err_cap, "Image exceeds 8K limit (8192x8192)");
        return -1;
    }

    uint64_t row_bytes = (((uint64_t)width * (uint64_t)bpp + 31ull) / 32ull) * 4ull;
    if ((uint64_t)bf_off >= file_size ||
        (uint64_t)bf_off + row_bytes * (uint64_t)height > file_size ||
        row_bytes > 65536ull) {
        snprintf(err, err_cap, "Truncated BMP pixel data");
        return -1;
    }

    uint32_t* pix = (uint32_t*)malloc((size_t)width * (size_t)height * 4u);
    uint8_t* row_buf = (uint8_t*)malloc((size_t)row_bytes);
    uint8_t* io_buf = (uint8_t*)malloc(65536u);
    if (!pix || !row_buf || !io_buf) {
        free(pix);
        free(row_buf);
        free(io_buf);
        snprintf(err, err_cap, "Out of Memory for BMP pixels");
        return -1;
    }

    uint32_t pal[256];
    memset(pal, 0, sizeof(pal));
    if (bpp <= 8) {
        uint64_t pal_off = 14ull + (uint64_t)dib_size;
        int colors = 1 << bpp;
        if (pal_off + (uint64_t)colors * 4ull <= file_size &&
            pxe_seek(fd, pal_off) == 0) {
            uint8_t ptmp[256 * 4];
            if (pxe_read(fd, ptmp, (size_t)colors * 4u) == colors * 4) {
                for (int i = 0; i < colors; i++) {
                    pal[i] = 0xFF000000u | ((uint32_t)ptmp[i * 4 + 2] << 16) |
                             ((uint32_t)ptmp[i * 4 + 1] << 8) | ptmp[i * 4 + 0];
                }
            }
        }
    }

    if (pxe_seek(fd, (uint64_t)bf_off) != 0) {
        free(pix);
        free(row_buf);
        free(io_buf);
        snprintf(err, err_cap, "BMP seek failed");
        return -1;
    }

    size_t io_pos = 0, io_len = 0;
    uint8_t any_alpha = 0;
    for (int sy = 0; sy < height; sy++) {
        if (stream_read_exact(fd, io_buf, 65536u, &io_pos, &io_len,
                              row_buf, (size_t)row_bytes) != 0) {
            free(pix);
            free(row_buf);
            free(io_buf);
            snprintf(err, err_cap, "BMP stream read failed");
            return -1;
        }
        int dst_y = top_down ? sy : (height - 1 - sy);
        if (bpp == 32) {
            for (int x = 0; x < width; x++) any_alpha |= row_buf[x * 4 + 3];
            bmp_decode_row(32, width, row_buf, pal, 1, pix + (size_t)dst_y * (size_t)width);
        } else {
            bmp_decode_row(bpp, width, row_buf, pal, 0, pix + (size_t)dst_y * (size_t)width);
        }
    }

    if (bpp == 32 && !any_alpha) {
        size_t npix = (size_t)width * (size_t)height;
        for (size_t i = 0; i < npix; i++) pix[i] |= 0xFF000000u;
    }

    free(row_buf);
    free(io_buf);
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

static int decode_png(const uint8_t* data, size_t size, viewer_img_t* out,
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

    size_t pos = 8;
    size_t idat_total = 0;
    while (pos + 12 <= size) {
        uint32_t clen = read_be32(data + pos);
        uint32_t ctyp = read_be32(data + pos + 4);
        if ((size_t)clen > size - pos - 12) break;
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
            idat_total += (size_t)clen;
        } else if (ctyp == 0x49454E44u) { /* IEND */
            break;
        }
        pos += 12u + (size_t)clen;
    }

    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 ||
        bit_depth != 8 || interlace != 0 || idat_total < 4) {
        snprintf(err, err_cap, "Unsupported PNG (max 8192x8192, 8b non-interlaced)");
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

    size_t stride = 1u + (size_t)width * (size_t)bpp;
    size_t raw_size = stride * (size_t)height;

    uint8_t* idat_buf = (uint8_t*)malloc(idat_total);
    uint8_t* raw_buf  = (uint8_t*)malloc(raw_size);
    if (!idat_buf || !raw_buf) {
        free(idat_buf);
        free(raw_buf);
        snprintf(err, err_cap, "Out of Memory decoding PNG");
        return -1;
    }

    pos = 8;
    size_t cpos = 0;
    while (pos + 12 <= size) {
        uint32_t clen = read_be32(data + pos);
        uint32_t ctyp = read_be32(data + pos + 4);
        if ((size_t)clen > size - pos - 12) break;
        if (ctyp == 0x49444154u) {
            memcpy(idat_buf + cpos, data + pos + 8, clen);
            cpos += (size_t)clen;
        } else if (ctyp == 0x49454E44u) {
            break;
        }
        pos += 12u + (size_t)clen;
    }

    size_t decomp_len = 0;
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
        size_t row_bytes = (size_t)width * (size_t)bpp;

        if (ftype == 1) {
            for (size_t i = (size_t)bpp; i < row_bytes; i++) cur[i] += cur[i - bpp];
        } else if (ftype == 2) {
            if (prev_row) {
                for (size_t i = 0; i < row_bytes; i++) cur[i] += prev_row[i];
            }
        } else if (ftype == 3) {
            for (size_t i = 0; i < row_bytes; i++) {
                int a = (i >= (size_t)bpp) ? cur[i - bpp] : 0;
                int b = prev_row ? prev_row[i] : 0;
                cur[i] += (uint8_t)((a + b) >> 1);
            }
        } else if (ftype == 4) {
            for (size_t i = 0; i < row_bytes; i++) {
                int a = (i >= (size_t)bpp) ? cur[i - bpp] : 0;
                int b = prev_row ? prev_row[i] : 0;
                int c = (prev_row && i >= (size_t)bpp) ? prev_row[i - bpp] : 0;
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

static const uint8_t k_jpg_zz[64] = {
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
    int      valid;
    int32_t  maxcode[17];
    int32_t  valoffset[17];
    uint8_t  vals[256];
    uint16_t lut[256];
} jpg_huff_t;

typedef struct {
    uint8_t  id;
    uint8_t  h;
    uint8_t  v;
    uint8_t  qsel;
    int      bw;
    int      bh;
    int16_t* coeffs;
} jpg_comp_t;

typedef struct {
    const uint8_t* src;
    size_t         pos;
    size_t         end;
    uint64_t       bitbuf;
    int            bitcnt;
    uint8_t        marker;
} jpg_br_t;

typedef struct {
    uint16_t   qtab[4][64];
    int        qvalid[4];
    jpg_huff_t dc_huff[4];
    jpg_huff_t ac_huff[4];
    jpg_comp_t comp[4];
    int        width;
    int        height;
    int        ncomp;
    int        max_h;
    int        max_v;
    int        mcu_cols;
    int        mcu_rows;
    int        progressive;
    int        dri;
    int        has_jfif;
    int        has_adobe;
    uint8_t    adobe_xform;
    uint8_t    mcu_px[4][16][64];
} jpg_ctx_t;

static uint8_t jpg_clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static int jpg_build_huff(jpg_huff_t* h, const uint8_t bits[16], const uint8_t* vals, int nvals) {
    memset(h, 0, sizeof(*h));
    if (nvals > 256) return -1;
    memcpy(h->vals, vals, (size_t)nvals);
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
                uint8_t sym = vals[vi++];
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

static void jpg_fill_bits(jpg_br_t* br) {
    while (br->bitcnt <= 32) {
        if (br->marker || br->pos >= br->end) break;
        uint8_t b = br->src[br->pos++];
        if (b == 0xFFu) {
            while (br->pos < br->end && br->src[br->pos] == 0xFFu) br->pos++;
            if (br->pos >= br->end) {
                br->marker = 0xD9u;
                break;
            }
            uint8_t b2 = br->src[br->pos++];
            if (b2 != 0x00u) {
                br->marker = b2;
                break;
            }
        }
        br->bitbuf = (br->bitbuf << 8) | (uint64_t)b;
        br->bitcnt += 8;
    }
}

static int jpg_get_bit(jpg_br_t* br) {
    if (br->bitcnt == 0) {
        jpg_fill_bits(br);
        if (br->bitcnt == 0) return -1;
    }
    br->bitcnt--;
    return (int)((br->bitbuf >> br->bitcnt) & 1ULL);
}

static int jpg_get_bits(jpg_br_t* br, int n) {
    if (n == 0) return 0;
    if (br->bitcnt < n) jpg_fill_bits(br);
    if (br->bitcnt < n) return -1;
    br->bitcnt -= n;
    return (int)((br->bitbuf >> br->bitcnt) & ((1ULL << n) - 1ULL));
}

static int jpg_huff_decode(jpg_br_t* br, const jpg_huff_t* h) {
    if (br->bitcnt < 16) jpg_fill_bits(br);
    if (br->bitcnt >= 8) {
        uint8_t idx8 = (uint8_t)(br->bitbuf >> (br->bitcnt - 8));
        uint16_t e = h->lut[idx8];
        if (e & 0x8000u) {
            br->bitcnt -= (int)((e >> 8) & 0x0Fu);
            return (int)(e & 0xFFu);
        }
    }
    int code = 0;
    for (int len = 1; len <= 16; len++) {
        int b = jpg_get_bit(br);
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

static int jpg_extend(int v, int s) {
    if (s == 0) return 0;
    int vt = 1 << (s - 1);
    if (v < vt) return v + (-(1 << s)) + 1;
    return v;
}

static void jpg_idct_8x8(const int32_t coeff[64], uint8_t out[64]) {
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
            out[y * 8 + x]       = jpg_clamp8(p0);
            out[(7 - y) * 8 + x] = jpg_clamp8(p1);
        }
    }
}

static void jpg_check_restart(jpg_br_t* br, int dri, int* mcus_left,
                              int dc_pred[4], int* eobrun) {
    if (dri <= 0) return;
    if (--(*mcus_left) > 0) return;
    br->bitcnt = 0;
    if (br->marker >= 0xD0u && br->marker <= 0xD7u) {
        br->marker = 0;
    } else if (br->marker == 0) {
        while (br->pos < br->end) {
            if (br->src[br->pos] == 0xFFu && br->pos + 1 < br->end) {
                uint8_t nb = br->src[br->pos + 1];
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

static int jpg_decode_block_direct(jpg_br_t* br, const jpg_huff_t* dc_h,
                                   const jpg_huff_t* ac_h, const uint16_t qt[64],
                                   int* dc_pred, int32_t out32[64]) {
    memset(out32, 0, sizeof(int32_t) * 64u);
    int s = jpg_huff_decode(br, dc_h);
    if (s < 0 || s > 15) return -1;
    int v = (s > 0) ? jpg_get_bits(br, s) : 0;
    if (v < 0) return -1;
    *dc_pred += jpg_extend(v, s);
    out32[0] = (int32_t)(*dc_pred) * (int32_t)qt[0];

    int k = 1;
    while (k < 64) {
        int rs = jpg_huff_decode(br, ac_h);
        if (rs < 0) return -1;
        int r = rs >> 4;
        int sz = rs & 0x0F;
        if (sz == 0) {
            if (r == 15) {
                k += 16;
            } else {
                break;
            }
        } else {
            k += r;
            if (k >= 64) return -1;
            int ac = jpg_get_bits(br, sz);
            if (ac < 0) return -1;
            out32[k_jpg_zz[k]] = (int32_t)jpg_extend(ac, sz) * (int32_t)qt[k];
            k++;
        }
    }
    return 0;
}

static int jpg_decode_scan_block(jpg_ctx_t* j, jpg_br_t* br, int16_t* blk,
                                 int td, int ta, int* dc_pred, int* eobrun,
                                 int prog, int ss, int se, int ah, int al) {
    if (!prog) {
        int s = jpg_huff_decode(br, &j->dc_huff[td]);
        if (s < 0 || s > 15) return -1;
        int v = (s > 0) ? jpg_get_bits(br, s) : 0;
        if (v < 0) return -1;
        *dc_pred += jpg_extend(v, s);
        blk[0] = (int16_t)(*dc_pred);
        int k = 1;
        while (k < 64) {
            int rs = jpg_huff_decode(br, &j->ac_huff[ta]);
            if (rs < 0) return -1;
            int r = rs >> 4;
            int sz = rs & 0x0F;
            if (sz == 0) {
                if (r == 15) k += 16;
                else break;
            } else {
                k += r;
                if (k >= 64) return -1;
                int ac = jpg_get_bits(br, sz);
                if (ac < 0) return -1;
                blk[k] = (int16_t)jpg_extend(ac, sz);
                k++;
            }
        }
        return 0;
    }

    if (ss == 0 && se == 0) {
        if (ah == 0) {
            int s = jpg_huff_decode(br, &j->dc_huff[td]);
            if (s < 0 || s > 15) return -1;
            int v = (s > 0) ? jpg_get_bits(br, s) : 0;
            if (v < 0) return -1;
            *dc_pred += jpg_extend(v, s);
            blk[0] = (int16_t)(*dc_pred << al);
        } else {
            int bit = jpg_get_bit(br);
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
            int rs = jpg_huff_decode(br, &j->ac_huff[ta]);
            if (rs < 0) return -1;
            int r = rs >> 4;
            int sz = rs & 0x0F;
            if (sz == 0) {
                if (r == 15) {
                    k += 16;
                } else {
                    int ext = (r > 0) ? jpg_get_bits(br, r) : 0;
                    if (ext < 0) return -1;
                    *eobrun = (1 << r) + ext - 1;
                    break;
                }
            } else {
                k += r;
                if (k > se) return -1;
                int ac = jpg_get_bits(br, sz);
                if (ac < 0) return -1;
                blk[k] = (int16_t)(jpg_extend(ac, sz) << al);
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
            int rs = jpg_huff_decode(br, &j->ac_huff[ta]);
            if (rs < 0) return -1;
            int r = rs >> 4;
            int sz = rs & 0x0F;
            int val = 0;
            if (sz == 0) {
                if (r < 15) {
                    int ext = (r > 0) ? jpg_get_bits(br, r) : 0;
                    if (ext < 0) return -1;
                    *eobrun = (1 << r) + ext;
                    break;
                }
            } else {
                if (sz != 1) return -1;
                int sbit = jpg_get_bit(br);
                if (sbit < 0) return -1;
                val = sbit ? p1 : m1;
            }
            while (k <= se) {
                if (blk[k] != 0) {
                    int rbit = jpg_get_bit(br);
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
                int rbit = jpg_get_bit(br);
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

static void jpg_write_mcu(const jpg_ctx_t* j, uint32_t* pix, int mx, int my) {
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
        uint32_t* dst = pix + (size_t)y * (size_t)j->width;
        for (int x = x0; x < x1; x++) {
            int lx = x - x0;
            uint8_t s[4] = { 0, 128, 128, 255 };
            for (int c = 0; c < j->ncomp; c++) {
                int cx = (lx * j->comp[c].h) / j->max_h;
                int cy = (ly * j->comp[c].v) / j->max_v;
                int blk_idx = (cy >> 3) * j->comp[c].h + (cx >> 3);
                s[c] = j->mcu_px[c][blk_idx][(cy & 7) * 8 + (cx & 7)];
            }
            if (j->ncomp == 1) {
                uint8_t v = s[0];
                dst[x] = 0xFF000000u | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
            } else if (j->ncomp == 3) {
                if (is_rgb) {
                    dst[x] = 0xFF000000u | ((uint32_t)s[0] << 16) |
                             ((uint32_t)s[1] << 8) | s[2];
                } else {
                    int yv = (int)s[0];
                    int cb = (int)s[1] - 128;
                    int cr = (int)s[2] - 128;
                    uint8_t r = jpg_clamp8(yv + ((91881 * cr + 32768) >> 16));
                    uint8_t g = jpg_clamp8(yv - ((22554 * cb + 46802 * cr + 32768) >> 16));
                    uint8_t b = jpg_clamp8(yv + ((116130 * cb + 32768) >> 16));
                    dst[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                }
            } else {
                int c0 = s[0], c1 = s[1], c2 = s[2], k = s[3];
                if (j->has_adobe && j->adobe_xform == 2) {
                    int yv = c0, cb = c1 - 128, cr = c2 - 128;
                    c0 = jpg_clamp8(yv + ((91881 * cr + 32768) >> 16));
                    c1 = jpg_clamp8(yv - ((22554 * cb + 46802 * cr + 32768) >> 16));
                    c2 = jpg_clamp8(yv + ((116130 * cb + 32768) >> 16));
                }
                uint8_t r = (uint8_t)((c0 * k) / 255);
                uint8_t g = (uint8_t)((c1 * k) / 255);
                uint8_t b = (uint8_t)((c2 * k) / 255);
                dst[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            }
        }
    }
}

static int decode_jpg(const uint8_t* data, size_t size, viewer_img_t* out,
                      char* err, size_t err_cap) {
    if (size < 4 || data[0] != 0xFFu || data[1] != 0xD8u) {
        snprintf(err, err_cap, "Invalid JPEG SOI signature");
        return -1;
    }

    jpg_ctx_t* j = (jpg_ctx_t*)calloc(1, sizeof(jpg_ctx_t));
    if (!j) {
        snprintf(err, err_cap, "Out of Memory for JPEG context");
        return -1;
    }

    uint32_t* pix = NULL;
    int rendered_direct = 0;
    size_t pos = 2;
    uint8_t next_m = 0;
    int rc = 0;

    while (1) {
        uint8_t m = 0;
        if (next_m) {
            m = next_m;
            next_m = 0;
        } else {
            while (pos < size && data[pos] != 0xFFu) pos++;
            while (pos < size && data[pos] == 0xFFu) pos++;
            if (pos >= size) break;
            m = data[pos++];
            if (m == 0x00u || (m >= 0xD0u && m <= 0xD7u) || m == 0xD8u || m == 0x01u) continue;
        }

        if (m == 0xD9u) break; /* EOI */
        if (pos + 2 > size) break;
        uint16_t seg_len = read_be16(data + pos);
        if (seg_len < 2 || pos + seg_len > size) {
            snprintf(err, err_cap, "Corrupt JPEG segment length");
            rc = -1;
            break;
        }
        const uint8_t* seg = data + pos + 2;
        size_t slen = (size_t)(seg_len - 2u);
        pos += seg_len;

        if (m == 0xDBu) { /* DQT */
            size_t i = 0;
            while (i < slen) {
                uint8_t pqtq = seg[i++];
                int pq = pqtq >> 4;
                int tq = pqtq & 0x0F;
                if (tq >= 4 || (pq != 0 && pq != 1)) { rc = -1; break; }
                size_t need = pq ? 128u : 64u;
                if (i + need > slen) { rc = -1; break; }
                for (int k = 0; k < 64; k++) {
                    j->qtab[tq][k] = pq ? read_be16(seg + i + k * 2) : (uint16_t)seg[i + k];
                }
                i += need;
                j->qvalid[tq] = 1;
            }
            if (rc != 0) { snprintf(err, err_cap, "Invalid JPEG DQT table"); break; }
        } else if (m == 0xC0u || m == 0xC1u || m == 0xC2u) { /* SOF0/1/2 */
            if (slen < 6) { snprintf(err, err_cap, "Invalid JPEG SOF"); rc = -1; break; }
            if (seg[0] != 8) {
                snprintf(err, err_cap, "Unsupported %d-bit JPEG", (int)seg[0]);
                rc = -1;
                break;
            }
            j->progressive = (m == 0xC2u);
            j->height = (int)read_be16(seg + 1);
            j->width  = (int)read_be16(seg + 3);
            j->ncomp  = (int)seg[5];
            if (j->width <= 0 || j->height <= 0 || j->width > 8192 || j->height > 8192) {
                snprintf(err, err_cap, "JPEG dimensions invalid or >8K");
                rc = -1;
                break;
            }
            if ((j->ncomp != 1 && j->ncomp != 3 && j->ncomp != 4) ||
                slen < (size_t)(6 + 3 * j->ncomp)) {
                snprintf(err, err_cap, "Unsupported JPEG component count");
                rc = -1;
                break;
            }
            j->max_h = 1;
            j->max_v = 1;
            for (int c = 0; c < j->ncomp; c++) {
                j->comp[c].id   = seg[6 + 3 * c];
                uint8_t hv      = seg[7 + 3 * c];
                j->comp[c].h    = (uint8_t)(hv >> 4);
                j->comp[c].v    = (uint8_t)(hv & 0x0F);
                j->comp[c].qsel = (uint8_t)(seg[8 + 3 * c] & 0x03);
                if (j->comp[c].h < 1 || j->comp[c].h > 4 ||
                    j->comp[c].v < 1 || j->comp[c].v > 4) {
                    snprintf(err, err_cap, "Unsupported JPEG sampling factor");
                    rc = -1;
                    break;
                }
                if (j->comp[c].h > j->max_h) j->max_h = j->comp[c].h;
                if (j->comp[c].v > j->max_v) j->max_v = j->comp[c].v;
            }
            if (rc != 0) break;
            int mcu_w = j->max_h * 8;
            int mcu_h = j->max_v * 8;
            j->mcu_cols = (j->width  + mcu_w - 1) / mcu_w;
            j->mcu_rows = (j->height + mcu_h - 1) / mcu_h;
            for (int c = 0; c < j->ncomp; c++) {
                j->comp[c].bw = j->mcu_cols * j->comp[c].h;
                j->comp[c].bh = j->mcu_rows * j->comp[c].v;
                if (j->progressive) {
                    size_t nblk = (size_t)j->comp[c].bw * (size_t)j->comp[c].bh;
                    j->comp[c].coeffs = (int16_t*)calloc(nblk * 64u, sizeof(int16_t));
                    if (!j->comp[c].coeffs) {
                        snprintf(err, err_cap, "Out of Memory for Progressive JPEG");
                        rc = -1;
                        break;
                    }
                }
            }
            if (rc != 0) break;
        } else if (m == 0xC4u) { /* DHT */
            size_t i = 0;
            while (i < slen) {
                if (i + 17 > slen) { rc = -1; break; }
                uint8_t tcth = seg[i++];
                int tc = tcth >> 4;
                int th = tcth & 0x0F;
                if (tc > 1 || th >= 4) { rc = -1; break; }
                const uint8_t* bits = seg + i;
                i += 16;
                int nvals = 0;
                for (int b = 0; b < 16; b++) nvals += bits[b];
                if (i + (size_t)nvals > slen) { rc = -1; break; }
                jpg_huff_t* tbl = tc ? &j->ac_huff[th] : &j->dc_huff[th];
                if (jpg_build_huff(tbl, bits, seg + i, nvals) != 0) { rc = -1; break; }
                i += (size_t)nvals;
            }
            if (rc != 0) { snprintf(err, err_cap, "Invalid JPEG Huffman table"); break; }
        } else if (m == 0xDDu) { /* DRI */
            if (slen >= 2) j->dri = (int)read_be16(seg);
        } else if (m == 0xE0u && slen >= 5 && memcmp(seg, "JFIF", 4) == 0) {
            j->has_jfif = 1;
        } else if (m == 0xEEu && slen >= 12 && memcmp(seg, "Adobe", 5) == 0) {
            j->has_adobe = 1;
            j->adobe_xform = seg[11];
        } else if (m == 0xDAu) { /* SOS */
            if (j->width <= 0 || j->ncomp <= 0 || slen < 1) {
                snprintf(err, err_cap, "JPEG SOS before SOF");
                rc = -1;
                break;
            }
            int ns = (int)seg[0];
            if (ns < 1 || ns > j->ncomp || slen < (size_t)(4 + 2 * ns)) {
                snprintf(err, err_cap, "Invalid JPEG SOS header");
                rc = -1;
                break;
            }
            int scan_c[4], scan_td[4], scan_ta[4];
            for (int k = 0; k < ns; k++) {
                uint8_t cs = seg[1 + 2 * k];
                uint8_t tdta = seg[2 + 2 * k];
                scan_td[k] = (int)(tdta >> 4);
                scan_ta[k] = (int)(tdta & 0x0F);
                int found = -1;
                for (int c = 0; c < j->ncomp; c++) {
                    if (j->comp[c].id == cs) { found = c; break; }
                }
                if (found < 0 || scan_td[k] >= 4 || scan_ta[k] >= 4) {
                    snprintf(err, err_cap, "Invalid JPEG scan component");
                    rc = -1;
                    break;
                }
                scan_c[k] = found;
            }
            if (rc != 0) break;

            int ss = (int)seg[1 + 2 * ns];
            int se = (int)seg[2 + 2 * ns];
            uint8_t ahal = seg[3 + 2 * ns];
            int ah = (int)(ahal >> 4);
            int al = (int)(ahal & 0x0F);
            if (ss > se || se >= 64) {
                snprintf(err, err_cap, "Invalid JPEG spectral bounds");
                rc = -1;
                break;
            }
            for (int k = 0; k < ns; k++) {
                if ((ss == 0 && ah == 0 && !j->dc_huff[scan_td[k]].valid) ||
                    (se > 0 && !j->ac_huff[scan_ta[k]].valid) ||
                    !j->qvalid[j->comp[scan_c[k]].qsel]) {
                    snprintf(err, err_cap, "Missing JPEG Huffman/Quant table");
                    rc = -1;
                    break;
                }
            }
            if (rc != 0) break;

            jpg_br_t br;
            memset(&br, 0, sizeof(br));
            br.src = data;
            br.pos = pos;
            br.end = size;

            int dc_pred[4] = { 0, 0, 0, 0 };
            int eobrun = 0;
            int mcus_left = j->dri;

            if (!j->progressive && ns == j->ncomp && !rendered_direct) {
                if (!pix) {
                    pix = (uint32_t*)malloc((size_t)j->width * (size_t)j->height * 4u);
                    if (!pix) {
                        snprintf(err, err_cap, "Out of Memory for JPEG pixels");
                        rc = -1;
                        break;
                    }
                }
                size_t total_mcus = (size_t)j->mcu_cols * (size_t)j->mcu_rows;
                size_t mcu_idx = 0;
                for (int my = 0; my < j->mcu_rows && rc == 0; my++) {
                    for (int mx = 0; mx < j->mcu_cols && rc == 0; mx++, mcu_idx++) {
                        for (int k = 0; k < ns && rc == 0; k++) {
                            int ci = scan_c[k];
                            jpg_comp_t* c = &j->comp[ci];
                            for (int v = 0; v < c->v && rc == 0; v++) {
                                for (int u = 0; u < c->h && rc == 0; u++) {
                                    int32_t blk32[64];
                                    if (jpg_decode_block_direct(&br, &j->dc_huff[scan_td[k]],
                                                                &j->ac_huff[scan_ta[k]],
                                                                j->qtab[c->qsel],
                                                                &dc_pred[k], blk32) != 0) {
                                        snprintf(err, err_cap, "JPEG entropy decode error");
                                        rc = -1;
                                        break;
                                    }
                                    jpg_idct_8x8(blk32, j->mcu_px[ci][v * c->h + u]);
                                }
                            }
                        }
                        if (rc == 0) {
                            jpg_write_mcu(j, pix, mx, my);
                            if (mcu_idx + 1 < total_mcus) {
                                jpg_check_restart(&br, j->dri, &mcus_left, dc_pred, &eobrun);
                            }
                        }
                    }
                }
                rendered_direct = 1;
            } else {
                for (int c = 0; c < j->ncomp; c++) {
                    if (!j->comp[c].coeffs) {
                        size_t nblk = (size_t)j->comp[c].bw * (size_t)j->comp[c].bh;
                        j->comp[c].coeffs = (int16_t*)calloc(nblk * 64u, sizeof(int16_t));
                        if (!j->comp[c].coeffs) {
                            snprintf(err, err_cap, "Out of Memory for JPEG coeffs");
                            rc = -1;
                            break;
                        }
                    }
                }
                if (rc != 0) break;

                if (ns == 1) {
                    int ci = scan_c[0];
                    jpg_comp_t* c = &j->comp[ci];
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
                            if (jpg_decode_scan_block(j, &br, blk, scan_td[0], scan_ta[0],
                                                      &dc_pred[0], &eobrun,
                                                      j->progressive, ss, se, ah, al) != 0) {
                                snprintf(err, err_cap, "JPEG scan decode error");
                                rc = -1;
                                break;
                            }
                            if (mcu_idx + 1 < total_mcus) {
                                jpg_check_restart(&br, j->dri, &mcus_left, dc_pred, &eobrun);
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
                                jpg_comp_t* c = &j->comp[ci];
                                for (int v = 0; v < c->v && rc == 0; v++) {
                                    for (int u = 0; u < c->h && rc == 0; u++) {
                                        size_t bx = (size_t)(mx * c->h + u);
                                        size_t by = (size_t)(my * c->v + v);
                                        int16_t* blk = c->coeffs + (by * (size_t)c->bw + bx) * 64u;
                                        if (jpg_decode_scan_block(j, &br, blk, scan_td[k], scan_ta[k],
                                                                  &dc_pred[k], &eobrun,
                                                                  j->progressive, ss, se, ah, al) != 0) {
                                            snprintf(err, err_cap, "JPEG interleaved scan error");
                                            rc = -1;
                                            break;
                                        }
                                    }
                                }
                            }
                            if (rc == 0 && mcu_idx + 1 < total_mcus) {
                                jpg_check_restart(&br, j->dri, &mcus_left, dc_pred, &eobrun);
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
        snprintf(err, err_cap, "Missing JPEG image frame");
        rc = -1;
    }

    if (rc == 0 && !rendered_direct) {
        for (int c = 0; c < j->ncomp; c++) {
            if (!j->comp[c].coeffs || !j->qvalid[j->comp[c].qsel]) {
                snprintf(err, err_cap, "Incomplete JPEG scan data");
                rc = -1;
                break;
            }
        }
        if (rc == 0) {
            pix = (uint32_t*)malloc((size_t)j->width * (size_t)j->height * 4u);
            if (!pix) {
                snprintf(err, err_cap, "Out of Memory for JPEG pixels");
                rc = -1;
            }
        }
        if (rc == 0) {
            for (int my = 0; my < j->mcu_rows; my++) {
                for (int mx = 0; mx < j->mcu_cols; mx++) {
                    for (int c = 0; c < j->ncomp; c++) {
                        jpg_comp_t* cp = &j->comp[c];
                        const uint16_t* qt = j->qtab[cp->qsel];
                        for (int v = 0; v < cp->v; v++) {
                            for (int u = 0; u < cp->h; u++) {
                                size_t bx = (size_t)(mx * cp->h + u);
                                size_t by = (size_t)(my * cp->v + v);
                                const int16_t* zz = cp->coeffs + (by * (size_t)cp->bw + bx) * 64u;
                                int32_t blk32[64];
                                for (int i = 0; i < 64; i++) {
                                    blk32[k_jpg_zz[i]] = (int32_t)zz[i] * (int32_t)qt[i];
                                }
                                jpg_idct_8x8(blk32, j->mcu_px[c][v * cp->h + u]);
                            }
                        }
                    }
                    jpg_write_mcu(j, pix, mx, my);
                }
            }
        }
    }

    for (int c = 0; c < 4; c++) {
        if (j->comp[c].coeffs) free(j->comp[c].coeffs);
    }

    if (rc == 0 && pix) {
        out->width = j->width;
        out->height = j->height;
        out->pixels = pix;
        free(j);
        return 0;
    }

    free(pix);
    free(j);
    return -1;
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
    if (pxe_stat(path, &st) != 0 || st.is_dir || st.size < 4) {
        vw->err_modal = 1;
        snprintf(vw->err_msg, sizeof(vw->err_msg), "File not found or unreadable.");
        pxe_beep();
        return;
    }

    if (st.size >= 1024u * 1024u) {
        pxe_win_set_title(vw->win_id, "Viewer - Loading...");
    }

    int fd = pxe_open(path);
    if (fd < 0) {
        vw->err_modal = 1;
        snprintf(vw->err_msg, sizeof(vw->err_msg), "Failed to open file.");
        pxe_beep();
        return;
    }

    uint8_t hdr[64];
    int r0 = pxe_read(fd, hdr, sizeof(hdr));
    if (r0 < 4) {
        pxe_close(fd);
        vw->err_modal = 1;
        snprintf(vw->err_msg, sizeof(vw->err_msg), "Failed to read file header.");
        pxe_beep();
        return;
    }

    char err[96];
    err[0] = '\0';
    int rc = -1;

    if (r0 >= 54 && hdr[0] == 'B' && hdr[1] == 'M' && st.size >= 4ull * 1024ull * 1024ull) {
        rc = decode_bmp_stream(fd, st.size, hdr, &vw->img, err, sizeof(err));
        pxe_close(fd);
    } else {
        uint8_t* raw = (uint8_t*)malloc((size_t)st.size);
        if (!raw) {
            pxe_close(fd);
            vw->err_modal = 1;
            snprintf(vw->err_msg, sizeof(vw->err_msg), "Out of Memory reading file.");
            pxe_beep();
            return;
        }
        memcpy(raw, hdr, (size_t)r0);
        uint64_t got = (uint64_t)r0;
        while (got < st.size) {
            uint64_t rem = st.size - got;
            size_t chunk = (rem > 1048576ull) ? 1048576u : (size_t)rem;
            int r = pxe_read(fd, raw + got, chunk);
            if (r <= 0) break;
            got += (uint64_t)r;
        }
        pxe_close(fd);

        if (got >= 8 && raw[0] == 'B' && raw[1] == 'M') {
            rc = decode_bmp(raw, (size_t)got, &vw->img, err, sizeof(err));
        } else if (got >= 8 && raw[0] == 0x89 && raw[1] == 'P' &&
                   raw[2] == 'N' && raw[3] == 'G') {
            rc = decode_png(raw, (size_t)got, &vw->img, err, sizeof(err));
        } else if (got >= 4 && raw[0] == 0xFFu && raw[1] == 0xD8u) {
            rc = decode_jpg(raw, (size_t)got, &vw->img, err, sizeof(err));
        } else {
            snprintf(err, sizeof(err), "Unsupported format (BMP/PNG/JPG).");
        }
        free(raw);
    }

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
        uint64_t kb = ((uint64_t)vw->img.width * (uint64_t)vw->img.height * 4ull) / 1024ull;
        if (vw->zoom_pct == 0) {
            snprintf(status, sizeof(status), "%dx%d 32-bit | Zoom: Fit | %llu KB",
                     vw->img.width, vw->img.height, (unsigned long long)kb);
        } else {
            snprintf(status, sizeof(status), "%dx%d 32-bit | Zoom: %d%% | %llu KB",
                     vw->img.width, vw->img.height, vw->zoom_pct, (unsigned long long)kb);
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
        pxe_draw_text(surf, dx + 8, dy + 3, "Open Image (BMP/PNG/JPG)", PXE_COL_WHITE);
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
                                             "Open BMP/PNG/JPG file first.");
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
