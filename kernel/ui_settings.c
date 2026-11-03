#include "settings.h"
#include "wallpaper.h"

void settings_init(void) {
    wallpaper_init();
}

void settings_load(void) {
    wallpaper_init();
}

void settings_save(void) {
}

void settings_set_resolution(int width, int height, int bpp) {
    (void)width;
    (void)height;
    (void)bpp;
}

void settings_set_pattern(int pattern) {
    (void)pattern;
}

void settings_set_wallpaper(const char* path, wallpaper_mode_t mode) {
    (void)path;
    wallpaper_set_mode(mode);
}

void settings_clear_wallpaper(void) {
    wallpaper_clear();
}
