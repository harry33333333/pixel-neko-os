#include "vplayer_yuv.h"
#include <emmintrin.h>

static inline uint8_t clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static inline uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/*
 * Fixed-point limited-range (TV 16..235 / 16..240) YUV -> RGB coefficients (scale 64):
 * BT.601: R = 1.164*(Y-16) + 1.596*(V-128)
 *         G = 1.164*(Y-16) - 0.392*(U-128) - 0.813*(V-128)
 *         B = 1.164*(Y-16) + 2.017*(U-128)
 * BT.709: R = 1.164*(Y-16) + 1.793*(V-128)
 *         G = 1.164*(Y-16) - 0.213*(U-128) - 0.533*(V-128)
 *         B = 1.164*(Y-16) + 2.112*(U-128)
 */
typedef struct {
    int16_t cy;
    int16_t crv;
    int16_t cgu;
    int16_t cgv;
    int16_t cbu;
} yuv_coeff_t;

static const yuv_coeff_t k_bt601 = { 74, 102, -25, -52, 129 };
static const yuv_coeff_t k_bt709 = { 74, 115, -14, -34, 135 };

/* SSE2 1:1 row converter: 8 pixels per iteration -> 0x00RRGGBB (BGRA32 in LE) */
static void yuv420_row_rgb32_sse2(uint32_t* dst,
                                  const uint8_t* y_ptr,
                                  const uint8_t* u_ptr,
                                  const uint8_t* v_ptr,
                                  int width,
                                  const yuv_coeff_t* c) {
    const __m128i zero  = _mm_setzero_si128();
    const __m128i y_off = _mm_set1_epi16(16);
    const __m128i uv_off = _mm_set1_epi16(128);
    const __m128i rnd   = _mm_set1_epi16(32);
    const __m128i k_cy  = _mm_set1_epi16(c->cy);
    const __m128i k_crv = _mm_set1_epi16(c->crv);
    const __m128i k_cgu = _mm_set1_epi16(c->cgu);
    const __m128i k_cgv = _mm_set1_epi16(c->cgv);
    const __m128i k_cbu = _mm_set1_epi16(c->cbu);

    int x = 0;
    for (; x + 8 <= width; x += 8) {
        /* Load 8 Y bytes -> 8x int16 */
        __m128i y8  = _mm_loadl_epi64((const __m128i*)(y_ptr + x));
        __m128i y16 = _mm_sub_epi16(_mm_unpacklo_epi8(y8, zero), y_off);
        y16 = _mm_max_epi16(y16, zero);

        /* Load 4 U and 4 V bytes -> duplicate each to 8x int16 */
        uint32_t u4_raw = *(const uint32_t*)(u_ptr + (x >> 1));
        uint32_t v4_raw = *(const uint32_t*)(v_ptr + (x >> 1));
        __m128i u4 = _mm_cvtsi32_si128((int)u4_raw);
        __m128i v4 = _mm_cvtsi32_si128((int)v4_raw);
        __m128i u8 = _mm_unpacklo_epi8(u4, u4);
        __m128i v8 = _mm_unpacklo_epi8(v4, v4);
        __m128i u16 = _mm_sub_epi16(_mm_unpacklo_epi8(u8, zero), uv_off);
        __m128i v16 = _mm_sub_epi16(_mm_unpacklo_epi8(v8, zero), uv_off);

        __m128i y_scaled = _mm_add_epi16(_mm_mullo_epi16(y16, k_cy), rnd);

        __m128i r16 = _mm_srai_epi16(_mm_add_epi16(y_scaled, _mm_mullo_epi16(v16, k_crv)), 6);
        __m128i g16 = _mm_srai_epi16(_mm_add_epi16(y_scaled,
                        _mm_add_epi16(_mm_mullo_epi16(u16, k_cgu), _mm_mullo_epi16(v16, k_cgv))), 6);
        __m128i b16 = _mm_srai_epi16(_mm_add_epi16(y_scaled, _mm_mullo_epi16(u16, k_cbu)), 6);

        __m128i r8 = _mm_packus_epi16(r16, zero);
        __m128i g8 = _mm_packus_epi16(g16, zero);
        __m128i b8 = _mm_packus_epi16(b16, zero);

        /* Pack as B, G, R, 0 (0x00RRGGBB in little-endian uint32_t) */
        __m128i bg = _mm_unpacklo_epi8(b8, g8);
        __m128i ra = _mm_unpacklo_epi8(r8, zero);
        __m128i px0 = _mm_unpacklo_epi16(bg, ra);
        __m128i px1 = _mm_unpackhi_epi16(bg, ra);

        _mm_storeu_si128((__m128i*)(dst + x), px0);
        _mm_storeu_si128((__m128i*)(dst + x + 4), px1);
    }

    for (; x < width; x++) {
        int yv = (int)y_ptr[x] - 16;
        if (yv < 0) yv = 0;
        int uv = (int)u_ptr[x >> 1] - 128;
        int vv = (int)v_ptr[x >> 1] - 128;
        int ys = yv * c->cy + 32;
        uint8_t r = clamp_u8((ys + vv * c->crv) >> 6);
        uint8_t g = clamp_u8((ys + uv * c->cgu + vv * c->cgv) >> 6);
        uint8_t b = clamp_u8((ys + uv * c->cbu) >> 6);
        dst[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

void vp_yuv420_to_surface(const pxe_win_surface_t* surf,
                          int dst_x, int dst_y, int dst_w, int dst_h,
                          const uint8_t* y_plane, int y_stride,
                          const uint8_t* u_plane, const uint8_t* v_plane, int uv_stride,
                          int src_w, int src_h,
                          int bt709) {
    if (!surf || !surf->pixels || !y_plane || !u_plane || !v_plane) return;
    if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;

    const yuv_coeff_t* c = bt709 ? &k_bt709 : &k_bt601;

    /* Clamp destination rect to surface bounds */
    int clip_x0 = dst_x < 0 ? 0 : dst_x;
    int clip_y0 = dst_y < 0 ? 0 : dst_y;
    int clip_x1 = dst_x + dst_w;
    int clip_y1 = dst_y + dst_h;
    if (clip_x1 > surf->width) clip_x1 = surf->width;
    if (clip_y1 > surf->height) clip_y1 = surf->height;
    if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1) return;

    /* Fast path: 1:1 RGB32 (e.g. native 1080p full-screen / 1:1 window) */
    if (surf->format == PXE_CANVAS_RGB32 && dst_w == src_w && dst_h == src_h) {
        int copy_w = clip_x1 - clip_x0;
        int sx0 = clip_x0 - dst_x;
        for (int dy = clip_y0; dy < clip_y1; dy++) {
            int sy = dy - dst_y;
            uint32_t* row = (uint32_t*)((uint8_t*)surf->pixels + (size_t)dy * (size_t)surf->pitch) + clip_x0;
            const uint8_t* yp = y_plane + (size_t)sy * (size_t)y_stride + sx0;
            const uint8_t* up = u_plane + (size_t)(sy >> 1) * (size_t)uv_stride + (sx0 >> 1);
            const uint8_t* vp = v_plane + (size_t)(sy >> 1) * (size_t)uv_stride + (sx0 >> 1);
            yuv420_row_rgb32_sse2(row, yp, up, vp, copy_w, c);
        }
        return;
    }

    /* Scaled path: 16.16 fixed-point horizontal/vertical mapping (zero extra buffer) */
    uint32_t x_step = (uint32_t)(((uint64_t)(uint32_t)src_w << 16) / (uint32_t)dst_w);
    uint32_t y_step = (uint32_t)(((uint64_t)(uint32_t)src_h << 16) / (uint32_t)dst_h);

    for (int dy = clip_y0; dy < clip_y1; dy++) {
        int sy = (int)(((uint64_t)(uint32_t)(dy - dst_y) * y_step) >> 16);
        if (sy >= src_h) sy = src_h - 1;
        const uint8_t* yp = y_plane + (size_t)sy * (size_t)y_stride;
        const uint8_t* up = u_plane + (size_t)(sy >> 1) * (size_t)uv_stride;
        const uint8_t* vp = v_plane + (size_t)(sy >> 1) * (size_t)uv_stride;

        uint32_t sx_fp = (uint32_t)(clip_x0 - dst_x) * x_step;
        if (surf->format == PXE_CANVAS_RGB32) {
            uint32_t* row = (uint32_t*)((uint8_t*)surf->pixels + (size_t)dy * (size_t)surf->pitch);
            for (int dx = clip_x0; dx < clip_x1; dx++) {
                int sx = (int)(sx_fp >> 16);
                sx_fp += x_step;
                if (sx >= src_w) sx = src_w - 1;
                int yv = (int)yp[sx] - 16;
                if (yv < 0) yv = 0;
                int uv = (int)up[sx >> 1] - 128;
                int vv = (int)vp[sx >> 1] - 128;
                int ys = yv * c->cy + 32;
                uint8_t r = clamp_u8((ys + vv * c->crv) >> 6);
                uint8_t g = clamp_u8((ys + uv * c->cgu + vv * c->cgv) >> 6);
                uint8_t b = clamp_u8((ys + uv * c->cbu) >> 6);
                row[dx] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        } else if (surf->format == PXE_CANVAS_RGB565) {
            uint16_t* row = (uint16_t*)((uint8_t*)surf->pixels + (size_t)dy * (size_t)surf->pitch);
            for (int dx = clip_x0; dx < clip_x1; dx++) {
                int sx = (int)(sx_fp >> 16);
                sx_fp += x_step;
                if (sx >= src_w) sx = src_w - 1;
                int yv = (int)yp[sx] - 16;
                if (yv < 0) yv = 0;
                int uv = (int)up[sx >> 1] - 128;
                int vv = (int)vp[sx >> 1] - 128;
                int ys = yv * c->cy + 32;
                uint8_t r = clamp_u8((ys + vv * c->crv) >> 6);
                uint8_t g = clamp_u8((ys + uv * c->cgu + vv * c->cgv) >> 6);
                uint8_t b = clamp_u8((ys + uv * c->cbu) >> 6);
                row[dx] = pack_rgb565(r, g, b);
            }
        }
    }
}

