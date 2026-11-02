#ifndef NVIDIA_G7X_H
#define NVIDIA_G7X_H

int nvidia_g7x_probe(void);
int nvidia_g7x_is_active(void);
const char* nvidia_g7x_get_name(void);
const char* nvidia_g7x_get_status(void);
unsigned int nvidia_g7x_get_vram_size(void);
int nvidia_g7x_supports_1280x1024(void);

void nvidia_g7x_present_rect(int x, int y, int width, int height);
void nvidia_g7x_fill_rect(int x, int y, int width, int height,
                          unsigned int color);
void nvidia_g7x_screen_blt(int sx, int sy, int dx, int dy,
                           int width, int height);
void nvidia_g7x_sync(void);

#endif
