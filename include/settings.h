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
settings_boot_mode_t settings_get_boot_mode(void);
void settings_init(void);
char settings_get_system_drive(void);
bool settings_load(void);
bool settings_save(void);
bool settings_storage_available(void);
bool settings_stage_boot_resolution(int width, int height, int bpp);
void settings_set_mouse_speed(int speed);
void settings_set_resolution(int w, int h, int bpp);
void settings_set_pattern(int pattern);
void settings_set_clock_options(bool show_date, bool show_seconds);
bool settings_get_clock_show_date(void);
bool settings_get_clock_show_seconds(void);
void settings_restore_default_wallpaper(void);
void settings_set_wallpaper(const char* path, wallpaper_mode_t mode);
void settings_clear_wallpaper(void);

#endif // SETTINGS_H
