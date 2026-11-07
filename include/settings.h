#ifndef SETTINGS_H
#define SETTINGS_H

#include "wallpaper.h"
#include <stdbool.h>

typedef enum {
    SETTINGS_BOOT_LIVE = 0,
    SETTINGS_BOOT_INSTALLER,
    SETTINGS_BOOT_LOCAL
} settings_boot_mode_t;

void settings_set_boot_mode(settings_boot_mode_t mode);
void settings_init(void);
char settings_get_system_drive(void);
bool settings_load(void);
bool settings_save(void);
bool settings_storage_available(void);
void settings_set_mouse_speed(int speed);
void settings_set_resolution(int w, int h, int bpp);
void settings_set_pattern(int pattern);
void settings_set_wallpaper(const char* path, wallpaper_mode_t mode);
void settings_clear_wallpaper(void);

#endif // SETTINGS_H
