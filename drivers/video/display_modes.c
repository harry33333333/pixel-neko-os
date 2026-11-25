#include "display_modes.h"
#include "log.h"
#include "mm.h"
#include "string.h"

extern volatile struct limine_framebuffer_request framebuffer_request;

static display_mode_t g_modes[DISPLAY_MODE_MAX];
static int g_mode_count;
static display_mode_t g_firmware_modes[DISPLAY_MODE_MAX];
static int g_firmware_mode_count;
static display_mode_t g_edid_modes[DISPLAY_MODE_MAX];
static int g_edid_mode_count;
static uint8_t g_edid[DISPLAY_EDID_MAX_BYTES];
static size_t g_edid_size;
static bool g_edid_valid;
static bool g_virtual_display;
static const char* g_mode_source = "Conservative fallback";

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t pixel_clock_khz;
    uint16_t h_total;
    uint16_t h_sync_start;
    uint16_t h_sync_end;
    uint16_t v_total;
    uint16_t v_sync_start;
    uint16_t v_sync_end;
    uint8_t h_positive;
    uint8_t v_positive;
} display_standard_timing_t;

static const display_standard_timing_t g_standard_timings[] = {
    {  640,  400,  25175,  800,  656,  720,  449,  407,  409, 0, 0 },
    {  720,  400,  28322,  900,  720,  738,  449,  412,  414, 0, 1 },
    {  640,  480,  25175,  800,  656,  752,  525,  490,  492, 0, 0 },
    {  800,  600,  40000, 1056,  840,  968,  628,  601,  605, 1, 1 },
    { 1024,  600,  51200, 1312, 1040, 1136,  624,  601,  604, 0, 0 },
    { 1024,  768,  65000, 1344, 1048, 1184,  806,  771,  777, 0, 0 },
    { 1152,  864, 108000, 1600, 1216, 1344,  900,  865,  868, 1, 1 },
    { 1280,  720,  74250, 1650, 1390, 1430,  750,  725,  730, 1, 1 },
    { 1280,  768,  79500, 1664, 1328, 1360,  798,  771,  778, 0, 0 },
    { 1280,  800,  83500, 1680, 1352, 1480,  831,  803,  809, 0, 0 },
    { 1280,  960, 108000, 1800, 1376, 1488, 1000,  961,  964, 1, 1 },
    { 1280, 1024, 108000, 1688, 1328, 1440, 1066, 1025, 1028, 1, 1 },
    { 1360,  768,  85500, 1792, 1432, 1536,  795,  768,  774, 1, 1 },
    { 1366,  768,  85500, 1792, 1436, 1572,  798,  771,  774, 1, 1 },
    { 1400, 1050, 121750, 1864, 1488, 1632, 1089, 1053, 1059, 0, 0 },
    { 1440,  900, 106500, 1904, 1520, 1672,  934,  903,  909, 0, 0 },
    { 1600,  900, 108000, 1800, 1624, 1704, 1000,  901,  904, 1, 1 },
    { 1600, 1200, 162000, 2160, 1664, 1856, 1250, 1201, 1204, 1, 1 },
    { 1680, 1050, 146250, 2240, 1784, 1960, 1089, 1053, 1059, 0, 0 },
    { 1920, 1080, 148500, 2200, 2008, 2052, 1125, 1084, 1089, 1, 1 },
    { 1920, 1200, 193250, 2592, 2052, 2156, 1245, 1206, 1212, 0, 0 },
    { 1920, 1440, 234000, 2600, 2048, 2256, 1500, 1441, 1444, 0, 0 },
    { 2560, 1440, 241500, 2720, 2608, 2640, 1481, 1443, 1449, 0, 0 },
    { 3840, 2160, 594000, 4400, 4016, 4104, 2250, 2168, 2178, 1, 1 }
};

static const void* display_limine_ptr(const void* pointer) {
    uintptr_t address = (uintptr_t)pointer;
    if (!address || !g_hhdm.hhdm_offset) return pointer;
    if (address < g_hhdm.hhdm_offset) {
        return (const void*)(address + g_hhdm.hhdm_offset);
    }
    return pointer;
}

static uint16_t display_le16(const uint8_t* value) {
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

static uint8_t display_bpp_mask(int bpp) {
    if (bpp == 8) return DISPLAY_BPP_MASK_8;
    if (bpp == 16) return DISPLAY_BPP_MASK_16;
    if (bpp == 24) return DISPLAY_BPP_MASK_24;
    if (bpp == 32) return DISPLAY_BPP_MASK_32;
    return 0;
}

static bool display_mode_valid(int width, int height, uint8_t bpp_mask) {
    return width >= 640 && height >= 480 && width <= 16384 &&
           height <= 16384 && (bpp_mask & DISPLAY_BPP_MASK_ALL) != 0;
}

static bool display_make_standard_timing(int width, int height,
                                         display_timing_t* out_timing) {
    if (!out_timing) return false;
    for (size_t index = 0;
         index < sizeof(g_standard_timings) / sizeof(g_standard_timings[0]);
         index++) {
        const display_standard_timing_t* source = &g_standard_timings[index];
        if (source->width != width || source->height != height) continue;
        memset(out_timing, 0, sizeof(*out_timing));
        out_timing->h_display = source->width;
        out_timing->h_total = source->h_total;
        out_timing->h_sync_start = source->h_sync_start;
        out_timing->h_sync_end = source->h_sync_end;
        out_timing->v_display = source->height;
        out_timing->v_total = source->v_total;
        out_timing->v_sync_start = source->v_sync_start;
        out_timing->v_sync_end = source->v_sync_end;
        out_timing->pixel_clock_khz = source->pixel_clock_khz;
        out_timing->h_sync_positive = source->h_positive;
        out_timing->v_sync_positive = source->v_positive;
        uint32_t divisor = (uint32_t)out_timing->h_total *
                           (uint32_t)out_timing->v_total;
        out_timing->refresh_hz =
            divisor ? (uint16_t)((out_timing->pixel_clock_khz * 1000U +
                                  divisor / 2U) / divisor) : 60U;
        return true;
    }
    return false;
}

bool display_modes_calc_cvt_rb(int width, int height, int refresh_hz,
                               display_timing_t* out_timing) {
    if (!out_timing || width < 320 || height < 200 || width > 16384 ||
        height > 16384) {
        return false;
    }
    if (refresh_hz < 24) refresh_hz = 60;
    if (refresh_hz > 360) refresh_hz = 360;

    /*
     * Integer-only CVT-RB (<= 100 Hz) / CVT-RBv2 (> 100 Hz) timing generator.
     * Both use 8-pixel aligned horizontal parameters for full Mach64/NV CRTC
     * compatibility while keeping pixel clocks minimal at 120/144/165/240 Hz.
     */
    bool rbv2 = (refresh_hz > 100);
    uint16_t h_disp = (uint16_t)width;
    uint16_t h_blank = rbv2 ? 80U : 160U;
    uint16_t h_fp    = rbv2 ? 8U  : 48U;
    uint16_t h_sync  = 32U;
    uint16_t h_total = (uint16_t)(((h_disp + 7U) & ~7U) + h_blank);

    /* Minimum 460 us vertical blanking interval */
    uint64_t denom = 1000000ULL - 460ULL * (uint64_t)(unsigned int)refresh_hz;
    if (denom < 100000ULL) denom = 100000ULL;
    uint64_t v_total_est = ((uint64_t)(unsigned int)height * 1000000ULL + denom - 1ULL) / denom;
    uint32_t v_blank = (v_total_est > (uint64_t)(unsigned int)height)
                           ? (uint32_t)(v_total_est - (uint64_t)(unsigned int)height)
                           : 16U;
    uint32_t min_vblank = rbv2 ? 16U : 23U;
    if (v_blank < min_vblank) v_blank = min_vblank;
    if (v_blank > 512U) v_blank = 512U;

    uint16_t v_sync = 8U;
    if (!rbv2) {
        if (width * 3 == height * 4) v_sync = 4U;
        else if (width * 9 == height * 16) v_sync = 5U;
        else if (width * 10 == height * 16) v_sync = 6U;
        else if (width * 4 == height * 5) v_sync = 7U;
        else v_sync = 6U;
    }
    uint16_t v_bp = 6U;
    uint16_t v_fp = (v_blank > (uint32_t)(v_sync + v_bp))
                        ? (uint16_t)(v_blank - v_sync - v_bp)
                        : 3U;
    if (!rbv2) {
        v_fp = 3U;
        if (v_blank < (uint32_t)(v_fp + v_sync + v_bp)) {
            v_blank = (uint32_t)(v_fp + v_sync + v_bp);
        }
    }
    uint16_t v_total = (uint16_t)((uint32_t)height + v_blank);

    uint64_t pclk_hz = (uint64_t)h_total * (uint64_t)v_total *
                       (uint64_t)(unsigned int)refresh_hz;
    uint32_t pclk_khz = (uint32_t)((pclk_hz + 500ULL) / 1000ULL);
    if (pclk_khz == 0) return false;

    memset(out_timing, 0, sizeof(*out_timing));
    out_timing->h_display       = h_disp;
    out_timing->h_total         = h_total;
    out_timing->h_sync_start    = (uint16_t)(h_disp + h_fp);
    out_timing->h_sync_end      = (uint16_t)(h_disp + h_fp + h_sync);
    out_timing->v_display       = (uint16_t)height;
    out_timing->v_total         = v_total;
    out_timing->v_sync_start    = (uint16_t)(height + v_fp);
    out_timing->v_sync_end      = (uint16_t)(height + v_fp + v_sync);
    out_timing->pixel_clock_khz = pclk_khz;
    out_timing->refresh_hz      = (uint16_t)refresh_hz;
    out_timing->h_sync_positive = 1U;
    out_timing->v_sync_positive = 0U;
    out_timing->flags           = rbv2 ? 2U : 1U;
    return true;
}

static void display_mode_add_timed(display_mode_t* modes, int* count,
                                   int width, int height, uint8_t bpp_mask,
                                   uint16_t refresh_hz,
                                   bool preferred, bool from_edid,
                                   const display_timing_t* detailed_timing) {
    if (!modes || !count || *count < 0 || *count >= DISPLAY_MODE_MAX ||
        !display_mode_valid(width, height, bpp_mask)) {
        return;
    }
    display_timing_t timing;
    bool has_timing = false;
    if (detailed_timing) {
        timing = *detailed_timing;
        has_timing = timing.pixel_clock_khz != 0 &&
                     timing.h_total >= timing.h_display &&
                     timing.v_total >= timing.v_display;
        if (has_timing && refresh_hz == 0) {
            refresh_hz = timing.refresh_hz;
        }
    }
    if (refresh_hz == 0) refresh_hz = 60U;
    if (!has_timing) {
        if (refresh_hz >= 58U && refresh_hz <= 62U) {
            has_timing = display_make_standard_timing(width, height, &timing);
        }
        if (!has_timing) {
            has_timing = display_modes_calc_cvt_rb(width, height,
                                                   (int)refresh_hz, &timing);
        }
    }
    for (int index = 0; index < *count; index++) {
        uint16_t existing_hz = modes[index].refresh_hz ? modes[index].refresh_hz : 60U;
        if (modes[index].width == (uint16_t)width &&
            modes[index].height == (uint16_t)height &&
            existing_hz == refresh_hz) {
            modes[index].bpp_mask |= bpp_mask;
            modes[index].preferred |= preferred ? 1U : 0U;
            modes[index].from_edid |= from_edid ? 1U : 0U;
            if (has_timing && (!modes[index].timing_valid ||
                               (detailed_timing && from_edid))) {
                modes[index].timing = timing;
                modes[index].timing_valid = 1U;
            }
            return;
        }
    }
    display_mode_t* mode = &modes[(*count)++];
    memset(mode, 0, sizeof(*mode));
    mode->width = (uint16_t)width;
    mode->height = (uint16_t)height;
    mode->bpp_mask = bpp_mask;
    mode->preferred = preferred ? 1U : 0U;
    mode->from_edid = from_edid ? 1U : 0U;
    mode->timing_valid = has_timing ? 1U : 0U;
    mode->refresh_hz = refresh_hz;
    if (has_timing) mode->timing = timing;
}

static void display_mode_add_hz(display_mode_t* modes, int* count,
                                int width, int height, uint8_t bpp_mask,
                                uint16_t refresh_hz,
                                bool preferred, bool from_edid) {
    display_mode_add_timed(modes, count, width, height, bpp_mask, refresh_hz,
                           preferred, from_edid, NULL);
}

static void display_mode_add(display_mode_t* modes, int* count,
                             int width, int height, uint8_t bpp_mask,
                             bool preferred, bool from_edid) {
    display_mode_add_timed(modes, count, width, height, bpp_mask, 60U,
                           preferred, from_edid, NULL);
}

static void display_modes_sort(display_mode_t* modes, int count) {
    for (int left = 0; left < count; left++) {
        for (int right = left + 1; right < count; right++) {
            uint32_t left_area = (uint32_t)modes[left].width * modes[left].height;
            uint32_t right_area = (uint32_t)modes[right].width * modes[right].height;
            uint16_t left_hz = modes[left].refresh_hz ? modes[left].refresh_hz : 60U;
            uint16_t right_hz = modes[right].refresh_hz ? modes[right].refresh_hz : 60U;
            if (right_area < left_area ||
                (right_area == left_area && modes[right].width < modes[left].width) ||
                (right_area == left_area && modes[right].width == modes[left].width &&
                 right_hz < left_hz)) {
                display_mode_t temporary = modes[left];
                modes[left] = modes[right];
                modes[right] = temporary;
            }
        }
    }
}

static bool edid_block_valid(const uint8_t* block) {
    if (!block) return false;
    uint8_t sum = 0;
    for (int index = 0; index < 128; index++) sum = (uint8_t)(sum + block[index]);
    return sum == 0;
}

static void edid_add_dtd(const uint8_t* dtd, bool preferred) {
    if (!dtd || display_le16(dtd) == 0 || (dtd[17] & 0x80U)) return;
    int width = dtd[2] | ((int)(dtd[4] & 0xF0U) << 4);
    int height = dtd[5] | ((int)(dtd[7] & 0xF0U) << 4);
    int h_blank = dtd[3] | ((int)(dtd[4] & 0x0FU) << 8);
    int v_blank = dtd[6] | ((int)(dtd[7] & 0x0FU) << 4);
    if (width <= 0 || height <= 0 || h_blank <= 0 || v_blank <= 0) return;

    display_timing_t timing;
    memset(&timing, 0, sizeof(timing));
    timing.h_display = (uint16_t)width;
    timing.h_total = (uint16_t)(width + h_blank);
    timing.h_sync_start = (uint16_t)(width +
        (dtd[8] | ((int)(dtd[11] & 0xC0U) << 2)));
    timing.h_sync_end = (uint16_t)(timing.h_sync_start +
        (dtd[9] | ((int)(dtd[11] & 0x30U) << 4)));
    timing.v_display = (uint16_t)height;
    timing.v_total = (uint16_t)(height + v_blank);
    timing.v_sync_start = (uint16_t)(height +
        (((int)(dtd[10] & 0xF0U) >> 4) |
         ((int)(dtd[11] & 0x0CU) << 2)));
    timing.v_sync_end = (uint16_t)(timing.v_sync_start +
        (dtd[10] & 0x0FU) + ((dtd[11] & 0x03U) << 4));
    timing.pixel_clock_khz = (uint32_t)display_le16(dtd) * 10U;
    timing.h_sync_positive = (dtd[17] & 0x02U) ? 1U : 0U;
    timing.v_sync_positive = (dtd[17] & 0x04U) ? 1U : 0U;
    timing.flags = 1U;
    uint32_t divisor = (uint32_t)timing.h_total * timing.v_total;
    timing.refresh_hz = divisor
        ? (uint16_t)((timing.pixel_clock_khz * 1000U + divisor / 2U) /
                     divisor)
        : 60U;
    display_mode_add_timed(g_edid_modes, &g_edid_mode_count, width, height,
                           DISPLAY_BPP_MASK_ALL, timing.refresh_hz,
                           preferred, true, &timing);
}

static void edid_add_established(const uint8_t* edid) {
    static const uint16_t k_width[] = {
        720, 720, 640, 640, 640, 640, 800, 800,
        800, 800, 832, 1024, 1024, 1024, 1024, 1280, 1152
    };
    static const uint16_t k_height[] = {
        400, 400, 480, 480, 480, 480, 600, 600,
        600, 600, 624, 768, 768, 768, 768, 1024, 870
    };
    static const uint16_t k_hz[] = {
        70, 88, 60, 67, 72, 75, 56, 60,
        72, 75, 75, 87, 60, 70, 75, 75, 75
    };
    static const uint8_t k_mask[] = {
        0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01,
        0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01, 0x80
    };
    for (int index = 0; index < 17; index++) {
        int byte_index = index < 8 ? 35 : (index < 16 ? 36 : 37);
        if ((edid[byte_index] & k_mask[index]) != 0) {
            if (index == 11) continue; /* 1024x768@87i interlaced */
            display_mode_add_hz(g_edid_modes, &g_edid_mode_count,
                                k_width[index], k_height[index],
                                DISPLAY_BPP_MASK_ALL, k_hz[index], false, true);
        }
    }
}

static void edid_add_standard(const uint8_t* edid) {
    int major = edid[18];
    int minor = edid[19];
    bool has_16_10 = (major > 1) || (major == 1 && minor >= 3);
    for (int index = 0; index < 8; index++) {
        uint8_t first = edid[38 + index * 2];
        uint8_t second = edid[39 + index * 2];
        if ((first == 0x01 && second == 0x01) ||
            (first == 0x00 && second == 0x00)) {
            continue;
        }
        int width = ((int)first + 31) * 8;
        int aspect = (second >> 6) & 0x03;
        int refresh_hz = (second & 0x3F) + 60;
        int height;
        if (aspect == 0) height = has_16_10 ? (width * 10) / 16 : width;
        else if (aspect == 1) height = (width * 3) / 4;
        else if (aspect == 2) height = (width * 4) / 5;
        else height = (width * 9) / 16;
        display_mode_add_hz(g_edid_modes, &g_edid_mode_count, width, height,
                            DISPLAY_BPP_MASK_ALL, (uint16_t)refresh_hz, false, true);
    }
}

static void edid_add_vic(uint8_t vic) {
    int width = 0;
    int height = 0;
    uint16_t refresh_hz = 60U;
    switch (vic) {
        case 1:  width = 640;  height = 480;  refresh_hz = 60U;  break;
        case 2:
        case 3:  width = 720;  height = 480;  refresh_hz = 60U;  break;
        case 4:  width = 1280; height = 720;  refresh_hz = 60U;  break;
        case 16: width = 1920; height = 1080; refresh_hz = 60U;  break;
        case 17:
        case 18: width = 720;  height = 576;  refresh_hz = 50U;  break;
        case 19: width = 1280; height = 720;  refresh_hz = 50U;  break;
        case 31: width = 1920; height = 1080; refresh_hz = 50U;  break;
        case 32:
        case 33:
        case 34: width = 1920; height = 1080; refresh_hz = 60U;  break;
        case 41: width = 1280; height = 720;  refresh_hz = 100U; break;
        case 47: width = 1280; height = 720;  refresh_hz = 120U; break;
        case 63: width = 1920; height = 1080; refresh_hz = 120U; break;
        case 64: width = 1920; height = 1080; refresh_hz = 100U; break;
        case 93:
        case 94:
        case 95:
        case 96:
        case 97: width = 3840; height = 2160; refresh_hz = 60U;  break;
        case 117: width = 3840; height = 2160; refresh_hz = 100U; break;
        case 118: width = 3840; height = 2160; refresh_hz = 120U; break;
        default: break;
    }
    if (width) {
        display_mode_add_hz(g_edid_modes, &g_edid_mode_count, width, height,
                            DISPLAY_BPP_MASK_ALL, refresh_hz, false, true);
    }
}

static void edid_parse_displayid_dtd(const uint8_t* p, bool is_type7) {
    if (!p) return;
    uint32_t raw_pclk = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    uint32_t pclk_khz = is_type7 ? (raw_pclk + 1U) : ((raw_pclk + 1U) * 10U);
    if (pclk_khz < 10000U || pclk_khz > 2000000U) return;
    /* Skip interlaced timings (bit 4 of flags byte p[3]) */
    if (p[3] & 0x10U) return;

    int h_active = (int)display_le16(p + 4) + 1;
    int h_blank  = (int)display_le16(p + 6) + 1;
    int h_fp     = (int)(p[8] | ((p[9] & 0x7FU) << 8)) + 1;
    uint8_t h_pos = (p[9] & 0x80U) ? 1U : 0U;
    int h_sw     = (int)display_le16(p + 10) + 1;

    int v_active = (int)display_le16(p + 12) + 1;
    int v_blank  = (int)display_le16(p + 14) + 1;
    int v_fp     = (int)(p[16] | ((p[17] & 0x7FU) << 8)) + 1;
    uint8_t v_pos = (p[17] & 0x80U) ? 1U : 0U;
    int v_sw     = (int)display_le16(p + 18) + 1;

    if (h_active < 640 || v_active < 480 || h_blank <= 0 || v_blank <= 0) return;

    display_timing_t timing;
    memset(&timing, 0, sizeof(timing));
    timing.h_display       = (uint16_t)h_active;
    timing.h_total         = (uint16_t)(h_active + h_blank);
    timing.h_sync_start    = (uint16_t)(h_active + h_fp);
    timing.h_sync_end      = (uint16_t)(h_active + h_fp + h_sw);
    timing.v_display       = (uint16_t)v_active;
    timing.v_total         = (uint16_t)(v_active + v_blank);
    timing.v_sync_start    = (uint16_t)(v_active + v_fp);
    timing.v_sync_end      = (uint16_t)(v_active + v_fp + v_sw);
    timing.pixel_clock_khz = pclk_khz;
    timing.h_sync_positive = h_pos;
    timing.v_sync_positive = v_pos;
    timing.flags           = is_type7 ? 4U : 3U;

    uint64_t divisor = (uint64_t)timing.h_total * (uint64_t)timing.v_total;
    timing.refresh_hz = divisor
        ? (uint16_t)(((uint64_t)timing.pixel_clock_khz * 1000ULL + divisor / 2ULL) / divisor)
        : 60U;
    if (timing.refresh_hz < 24U || timing.refresh_hz > 360U) return;

    bool preferred = (p[3] & 0x80U) != 0;
    display_mode_add_timed(g_edid_modes, &g_edid_mode_count, h_active, v_active,
                           DISPLAY_BPP_MASK_ALL, timing.refresh_hz,
                           preferred, true, &timing);
}

static void edid_add_displayid_extension(const uint8_t* block) {
    if (!block || block[0] != 0x70U) return;
    /* DisplayID section starts at offset 1: [ver, len, prod_type, ext_count, blocks...] */
    int sec_len = (int)block[2];
    if (sec_len <= 0 || 5 + sec_len > 127) {
        sec_len = 122;
    }
    int cursor = 5;
    int end = 5 + sec_len;
    if (end > 127) end = 127;
    while (cursor + 3 <= end) {
        uint8_t tag = block[cursor];
        int len = (int)block[cursor + 2];
        cursor += 3;
        if (cursor + len > end) break;
        if (tag == 0x03U || tag == 0x22U) {
            bool is_type7 = (tag == 0x22U);
            for (int off = 0; off + 20 <= len; off += 20) {
                edid_parse_displayid_dtd(block + cursor + off, is_type7);
            }
        }
        cursor += len;
    }
}

static void edid_add_extension(const uint8_t* block) {
    if (!block) return;
    if (block[0] == 0x70U) {
        edid_add_displayid_extension(block);
        return;
    }
    if (block[0] != 0x02U) return;
    int dtd_offset = block[2];
    if (dtd_offset == 0) return;
    if (dtd_offset > 127) dtd_offset = 127;
    if (dtd_offset > 4) {
        int cursor = 4;
        while (cursor < dtd_offset) {
            uint8_t header = block[cursor];
            int length = header & 0x1FU;
            int tag = (header >> 5) & 0x07;
            if (cursor + 1 + length > dtd_offset ||
                cursor + 1 + length > 127) {
                break;
            }
            if (tag == 2) {
                for (int index = 0; index < length; index++) {
                    edid_add_vic(block[cursor + 1 + index] & 0x7FU);
                }
            }
            cursor += length + 1;
        }
    }
    if (dtd_offset >= 4) {
        for (int offset = dtd_offset; offset + 18 <= 127; offset += 18) {
            edid_add_dtd(block + offset, false);
        }
    }
}

static void display_parse_edid(const uint8_t* edid, size_t size) {
    static const uint8_t k_header[8] = {
        0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
    };
    g_edid_valid = false;
    g_edid_mode_count = 0;
    if (!edid || size < 128 || memcmp(edid, k_header, sizeof(k_header)) != 0 ||
        !edid_block_valid(edid)) {
        return;
    }
    g_edid_valid = true;
    edid_add_established(edid);
    edid_add_standard(edid);
    bool preferred = true;
    for (int offset = 54; offset <= 108; offset += 18) {
        if (display_le16(edid + offset) == 0) continue;
        edid_add_dtd(edid + offset, preferred);
        preferred = false;
    }
    size_t ext_limit = 128u + (size_t)edid[126] * 128u;
    if (ext_limit > size) ext_limit = size;
    if (ext_limit > DISPLAY_EDID_MAX_BYTES) ext_limit = DISPLAY_EDID_MAX_BYTES;
    for (size_t offset = 128; offset + 128 <= ext_limit; offset += 128) {
        if (!edid_block_valid(edid + offset)) continue;
        edid_add_extension(edid + offset);
    }
    display_modes_sort(g_edid_modes, g_edid_mode_count);
}

static void display_add_virtual_presets(display_mode_t* modes, int* count) {
    static const uint16_t k_width[] = {
        640, 800, 1024, 1280, 1366, 1600, 1920, 2560, 3840
    };
    static const uint16_t k_height[] = {
        480, 600, 768, 720, 768, 900, 1080, 1440, 2160
    };
    for (size_t index = 0;
         index < sizeof(k_width) / sizeof(k_width[0]); index++) {
        display_mode_add(modes, count, k_width[index], k_height[index],
                         DISPLAY_BPP_MASK_ALL, false, false);
    }
}

static void display_add_conservative_presets(display_mode_t* modes, int* count) {
    static const uint16_t k_width[] = { 800, 1024 };
    static const uint16_t k_height[] = { 600, 768 };
    for (size_t index = 0;
         index < sizeof(k_width) / sizeof(k_width[0]); index++) {
        display_mode_add(modes, count, k_width[index], k_height[index],
                         DISPLAY_BPP_MASK_16, false, false);
    }
}

static void display_capture_firmware_modes(const struct limine_framebuffer* fb) {
    g_firmware_mode_count = 0;
    if (!fb) return;
    bool has_rev1_modes = framebuffer_request.response &&
                          framebuffer_request.response->revision >= 1 &&
                          fb->modes != NULL;
    if (has_rev1_modes) {
        struct limine_video_mode* const* modes =
            (struct limine_video_mode* const*)display_limine_ptr(fb->modes);
        uint64_t count = fb->mode_count;
        if (count > DISPLAY_MODE_MAX) count = DISPLAY_MODE_MAX;
        for (uint64_t index = 0; index < count; index++) {
            const struct limine_video_mode* mode =
                (const struct limine_video_mode*)display_limine_ptr(modes[index]);
            if (!mode) continue;
            uint8_t mask = display_bpp_mask(mode->bpp);
            bool preferred = mode->width == fb->width &&
                             mode->height == fb->height &&
                             mode->bpp == fb->bpp;
            display_mode_add(g_firmware_modes, &g_firmware_mode_count,
                             (int)mode->width, (int)mode->height, mask,
                             preferred, false);
        }
    }
    display_mode_add(g_firmware_modes, &g_firmware_mode_count,
                     (int)fb->width, (int)fb->height,
                     display_bpp_mask(fb->bpp), true, false);
    display_modes_sort(g_firmware_modes, g_firmware_mode_count);
}

static void display_rebuild_catalog(void) {
    g_mode_count = 0;
    if (g_virtual_display) {
        for (int index = 0; index < g_firmware_mode_count; index++) {
            g_modes[g_mode_count++] = g_firmware_modes[index];
        }
        if (g_mode_count == 0) {
            display_add_virtual_presets(g_modes, &g_mode_count);
        }
        g_mode_source = g_firmware_mode_count ?
                        "Virtual firmware mode table" :
                        "Virtual generic mode table";
    } else if (g_edid_valid && g_edid_mode_count > 0) {
        for (int index = 0; index < g_edid_mode_count; index++) {
            g_modes[g_mode_count++] = g_edid_modes[index];
        }
        g_mode_source = "EDID 1.3/1.4 timings";
    } else if (g_firmware_mode_count > 0) {
        for (int index = 0; index < g_firmware_mode_count; index++) {
            g_modes[g_mode_count++] = g_firmware_modes[index];
        }
        g_mode_source = "UEFI GOP / legacy VBE mode table";
    } else {
        display_add_conservative_presets(g_modes, &g_mode_count);
        g_mode_source = "Conservative fallback mode table";
    }
    for (int index = 0; index < g_firmware_mode_count; index++) {
        if (!g_firmware_modes[index].preferred) continue;
        display_mode_add(g_modes, &g_mode_count,
                         g_firmware_modes[index].width,
                         g_firmware_modes[index].height,
                         g_firmware_modes[index].bpp_mask, true, false);
    }
    display_modes_sort(g_modes, g_mode_count);
}

bool display_modes_init(const struct limine_framebuffer* fb) {
    memset(g_modes, 0, sizeof(g_modes));
    memset(g_firmware_modes, 0, sizeof(g_firmware_modes));
    memset(g_edid_modes, 0, sizeof(g_edid_modes));
    memset(g_edid, 0, sizeof(g_edid));
    g_mode_count = 0;
    g_firmware_mode_count = 0;
    g_edid_mode_count = 0;
    g_edid_size = 0;
    g_edid_valid = false;
    g_virtual_display = false;
    g_mode_source = "Conservative fallback";
    if (!fb) return false;

    display_capture_firmware_modes(fb);
    if (fb->edid && fb->edid_size >= 128) {
        size_t copy_size = (size_t)fb->edid_size;
        if (copy_size > sizeof(g_edid)) copy_size = sizeof(g_edid);
        memcpy(g_edid, display_limine_ptr(fb->edid), copy_size);
        g_edid_size = copy_size;
        display_parse_edid(g_edid, g_edid_size);
    }
    display_rebuild_catalog();
    log_printf("[DISP] %s: %d modes, EDID=%s (%u bytes), firmware modes=%d\n",
               g_mode_source, g_mode_count, g_edid_valid ? "valid" : "missing",
               (unsigned)g_edid_size, g_firmware_mode_count);
    return g_mode_count > 0;
}

void display_modes_set_virtual(bool virtual_display) {
    if (g_virtual_display == virtual_display) return;
    g_virtual_display = virtual_display;
    display_rebuild_catalog();
    log_printf("[DISP] %s virtual display policy: %d modes\n",
               virtual_display ? "Enabled" : "Disabled", g_mode_count);
}

int display_modes_count(void) {
    return g_mode_count;
}

bool display_modes_get(int index, display_mode_t* out_mode) {
    if (!out_mode || index < 0 || index >= g_mode_count) return false;
    *out_mode = g_modes[index];
    return true;
}

bool display_modes_timing_ex(int width, int height, int refresh_hz,
                             display_timing_t* out_timing) {
    if (!out_timing || width <= 0 || height <= 0) return false;
    if (refresh_hz <= 0) refresh_hz = 60;
    for (int index = 0; index < g_mode_count; index++) {
        uint16_t mode_hz = g_modes[index].refresh_hz ? g_modes[index].refresh_hz : 60U;
        if (g_modes[index].width == (uint16_t)width &&
            g_modes[index].height == (uint16_t)height &&
            (int)mode_hz == refresh_hz &&
            g_modes[index].timing_valid) {
            *out_timing = g_modes[index].timing;
            return true;
        }
    }
    if (refresh_hz >= 58 && refresh_hz <= 62) {
        if (display_make_standard_timing(width, height, out_timing)) {
            return true;
        }
    }
    return display_modes_calc_cvt_rb(width, height, refresh_hz, out_timing);
}

bool display_modes_timing(int width, int height, display_timing_t* out_timing) {
    if (!out_timing || width <= 0 || height <= 0) return false;
    for (int index = 0; index < g_mode_count; index++) {
        if (g_modes[index].width == (uint16_t)width &&
            g_modes[index].height == (uint16_t)height) {
            if (g_modes[index].timing_valid) {
                *out_timing = g_modes[index].timing;
                return true;
            }
            break;
        }
    }
    if (display_make_standard_timing(width, height, out_timing)) {
        return true;
    }
    return display_modes_calc_cvt_rb(width, height, 60, out_timing);
}

bool display_modes_has_edid(void) {
    return !g_virtual_display && g_edid_valid;
}

const uint8_t* display_modes_edid(size_t* out_size) {
    if (out_size) *out_size = display_modes_has_edid() ? g_edid_size : 0;
    return display_modes_has_edid() ? g_edid : NULL;
}

bool display_modes_is_virtual(void) {
    return g_virtual_display;
}

const char* display_modes_source(void) {
    return g_mode_source;
}

int display_modes_max_width(void) {
    int maximum = 0;
    for (int index = 0; index < g_mode_count; index++) {
        if ((int)g_modes[index].width > maximum) maximum = g_modes[index].width;
    }
    return maximum;
}

int display_modes_max_height(void) {
    int maximum = 0;
    for (int index = 0; index < g_mode_count; index++) {
        if ((int)g_modes[index].height > maximum) maximum = g_modes[index].height;
    }
    return maximum;
}

uint8_t display_modes_bpp_mask(int width, int height) {
    for (int index = 0; index < g_mode_count; index++) {
        if (g_modes[index].width == width && g_modes[index].height == height) {
            return g_modes[index].bpp_mask;
        }
    }
    return 0;
}
