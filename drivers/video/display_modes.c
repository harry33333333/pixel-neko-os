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

static void display_mode_add_timed(display_mode_t* modes, int* count,
                                   int width, int height, uint8_t bpp_mask,
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
    }
    if (!has_timing) {
        has_timing = display_make_standard_timing(width, height, &timing);
    }
    for (int index = 0; index < *count; index++) {
        if (modes[index].width == (uint16_t)width &&
            modes[index].height == (uint16_t)height) {
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
    if (has_timing) mode->timing = timing;
}

static void display_mode_add(display_mode_t* modes, int* count,
                             int width, int height, uint8_t bpp_mask,
                             bool preferred, bool from_edid) {
    display_mode_add_timed(modes, count, width, height, bpp_mask,
                           preferred, from_edid, NULL);
}

static void display_modes_sort(display_mode_t* modes, int count) {
    for (int left = 0; left < count; left++) {
        for (int right = left + 1; right < count; right++) {
            uint32_t left_area = (uint32_t)modes[left].width * modes[left].height;
            uint32_t right_area = (uint32_t)modes[right].width * modes[right].height;
            if (right_area < left_area ||
                (right_area == left_area && modes[right].width < modes[left].width)) {
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
                           DISPLAY_BPP_MASK_ALL, preferred, true, &timing);
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
    static const uint8_t k_mask[] = {
        0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01,
        0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01, 0x80
    };
    for (int index = 0; index < 17; index++) {
        int byte_index = index < 8 ? 35 : (index < 16 ? 36 : 37);
        if ((edid[byte_index] & k_mask[index]) != 0) {
            display_mode_add(g_edid_modes, &g_edid_mode_count,
                             k_width[index], k_height[index],
                             DISPLAY_BPP_MASK_ALL, false, true);
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
        int height;
        if (aspect == 0) height = has_16_10 ? (width * 10) / 16 : width;
        else if (aspect == 1) height = (width * 3) / 4;
        else if (aspect == 2) height = (width * 4) / 5;
        else height = (width * 9) / 16;
        display_mode_add(g_edid_modes, &g_edid_mode_count, width, height,
                         DISPLAY_BPP_MASK_ALL, false, true);
    }
}

static void edid_add_vic(uint8_t vic) {
    int width = 0;
    int height = 0;
    switch (vic) {
        case 1:  width = 640; height = 480; break;
        case 2:
        case 3:  width = 720; height = 480; break;
        case 4:  width = 1280; height = 720; break;
        case 16: width = 1920; height = 1080; break;
        case 17:
        case 18: width = 720; height = 576; break;
        case 19: width = 1280; height = 720; break;
        case 31:
        case 32:
        case 33:
        case 34: width = 1920; height = 1080; break;
        case 93:
        case 94:
        case 95:
        case 96:
        case 97: width = 3840; height = 2160; break;
        default: break;
    }
    if (width) {
        display_mode_add(g_edid_modes, &g_edid_mode_count, width, height,
                         DISPLAY_BPP_MASK_ALL, false, true);
    }
}

static void edid_add_extension(const uint8_t* block) {
    if (!block || block[0] != 0x02) return;
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
    return display_make_standard_timing(width, height, out_timing);
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
