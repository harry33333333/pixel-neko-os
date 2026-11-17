#ifndef DISPLAY_MODES_H
#define DISPLAY_MODES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "limine.h"

#define DISPLAY_MODE_MAX        64
#define DISPLAY_EDID_MAX_BYTES  512

#define DISPLAY_BPP_MASK_8      (1U << 0)
#define DISPLAY_BPP_MASK_16     (1U << 1)
#define DISPLAY_BPP_MASK_24     (1U << 2)
#define DISPLAY_BPP_MASK_32     (1U << 3)
#define DISPLAY_BPP_MASK_ALL    (DISPLAY_BPP_MASK_8 | DISPLAY_BPP_MASK_16 | \
                                 DISPLAY_BPP_MASK_24 | DISPLAY_BPP_MASK_32)

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  bpp_mask;
    uint8_t  preferred;
    uint8_t  from_edid;
    uint8_t  reserved;
} display_mode_t;

bool display_modes_init(const struct limine_framebuffer* fb);
void display_modes_set_virtual(bool virtual_display);
int display_modes_count(void);
bool display_modes_get(int index, display_mode_t* out_mode);
bool display_modes_has_edid(void);
const uint8_t* display_modes_edid(size_t* out_size);
bool display_modes_is_virtual(void);
const char* display_modes_source(void);
int display_modes_max_width(void);
int display_modes_max_height(void);
uint8_t display_modes_bpp_mask(int width, int height);

#endif
