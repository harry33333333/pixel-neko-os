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

static void display_mode_add(display_mode_t* modes, int* count,
                             int width, int height, uint8_t bpp_mask,
                             bool preferred, bool from_edid) {
    if (!modes || !count || *count < 0 || *count >= DISPLAY_MODE_MAX ||
        !display_mode_valid(width, height, bpp_mask)) {
        return;
    }
    for (int index = 0; index < *count; index++) {
        if (modes[index].width == (uint16_t)width &&
            modes[index].height == (uint16_t)height) {
            modes[index].bpp_mask |= bpp_mask;
            modes[index].preferred |= preferred ? 1U : 0U;
            modes[index].from_edid |= from_edid ? 1U : 0U;
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
    display_mode_add(g_edid_modes, &g_edid_mode_count, width, height,
                     DISPLAY_BPP_MASK_ALL, preferred, true);
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
    static const uint16_t k_width[] = { 640, 800, 1024 };
    static const uint16_t k_height[] = { 480, 600, 768 };
    for (size_t index = 0;
         index < sizeof(k_width) / sizeof(k_width[0]); index++) {
        display_mode_add(modes, count, k_width[index], k_height[index],
                         DISPLAY_BPP_MASK_ALL, false, false);
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
