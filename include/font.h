#ifndef FONT_H
#define FONT_H

extern unsigned char font_small[128][7];
extern unsigned char font_8x16[128][16];

void font_draw_char(unsigned char* vga, int x, int y, char c, unsigned char fg);
void font_draw_text(unsigned char* vga, int x, int y, const char* text, unsigned char fg);

#endif