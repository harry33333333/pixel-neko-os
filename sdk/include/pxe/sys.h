#ifndef _PXE_SYS_H
#define _PXE_SYS_H

#include "abi.h"

void     pxe_exit(int status) __attribute__((noreturn));
int      pxe_getpid(void);
void     pxe_yield(void);
void     pxe_sleep_ms(uint32_t ms);
uint64_t pxe_get_ticks(void);
int      pxe_spawn(const char* path, int argc, const char* const* argv);
int      pxe_waitpid(int pid, int* out_status);
void     pxe_debug_log(const char* msg);

int      pxe_get_rtc_time(pxe_rtc_time_t* out);
int      pxe_sys_get_basic_info(pxe_basic_sysinfo_t* out);
int      pxe_display_get_caps(pxe_display_caps_t* out);

/* Settings control (requires PXE_FLAG_SYS_SETTINGS in Ring 3 or Ring 0) */
int      pxe_display_set_mode(int width, int height, int bpp);
int      pxe_display_apply_mode(int width, int height, int bpp);
int      pxe_desktop_set_pattern(int pattern);
int      pxe_desktop_set_wallpaper(const char* path, int mode);
int      pxe_desktop_set_wallpaper_mode(int mode);
int      pxe_desktop_clear_wallpaper(void);
int      pxe_desktop_restore_default_wallpaper(void);
int      pxe_clock_set_options(int show_date, int show_seconds);
int      pxe_mouse_set_speed(int speed);
int      pxe_set_language(int lang);
int      pxe_is_zh(void);
int      pxe_settings_save(void);
int      pxe_system_reboot(void);
int      pxe_system_shutdown(void);
int      pxe_kill_pid(uint32_t pid);
int      pxe_beep_tone(int freq_hz, int duration_ms);
int      pxe_desktop_shell_ctl(int op);
int      pxe_ime_set_cursor_pos(int client_x, int client_y);
int      pxe_notify_desktop(void);
int      pxe_get_ime_phrase_count(void);

#endif /* _PXE_SYS_H */

