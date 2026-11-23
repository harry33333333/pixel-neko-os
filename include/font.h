#ifndef FONT_H
#define FONT_H

#include <stdint.h>
#include <stddef.h>

#define NKFT_MAGIC   0x54464B4Eu /* "NKFT" */
#define NKFT_VERSION 1u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t glyph_count;
    uint32_t cp_offset;
    uint32_t g16_offset;
    uint32_t g12_offset;
    uint32_t total_size;
    uint32_t reserved;
} __attribute__((packed)) nkft_header_t;

extern unsigned char font_small[128][7];
extern unsigned char font_8x16[128][16];

void font_cjk_init(void);
const void* font_cjk_get_table(void);
uint64_t font_cjk_get_phys(void);
size_t font_cjk_get_pages(void);

int font_text_width(const char* text);
int font_text_width_small(const char* text);

void font_draw_char(unsigned char* vga, int x, int y, char c, unsigned char fg);
void font_draw_codepoint(unsigned char* vga, int x, int y, uint32_t cp, unsigned char fg);
void font_draw_text(unsigned char* vga, int x, int y, const char* text, unsigned char fg);
void font_draw_text_small(unsigned char* vga, int x, int y, const char* text, unsigned char fg);

#endif
