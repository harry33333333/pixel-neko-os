#ifndef _PXE_ABI_H
#define _PXE_ABI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Pixel Neko OS Shared Application ABI (Ring 3 & Ring 0 Unified Types)
 */

/* Classic 16-color VGA / Mac Platinum base palette indices */
#define PXE_COL_BLACK        0
#define PXE_COL_DARK_BLUE    1
#define PXE_COL_DARK_GREEN   2
#define PXE_COL_DARK_CYAN    3
#define PXE_COL_DARK_RED     4
#define PXE_COL_DARK_MAGENTA 5
#define PXE_COL_BROWN        6
#define PXE_COL_LIGHT_GRAY   7
#define PXE_COL_DARK_GRAY    8
#define PXE_COL_BLUE         9
#define PXE_COL_GREEN        10
#define PXE_COL_CYAN         11
#define PXE_COL_RED          12
#define PXE_COL_MAGENTA      13
#define PXE_COL_YELLOW       14
#define PXE_COL_WHITE        15
#define PXE_COL_TRANSPARENT  255

/* Virtual keycodes matching drivers/input/keyboard.h */
#define PXE_KEY_BACKSPACE    8
#define PXE_KEY_TAB          9
#define PXE_KEY_ENTER        10
#define PXE_KEY_ESC          27
#define PXE_KEY_UP           0x100
#define PXE_KEY_DOWN         0x101
#define PXE_KEY_LEFT         0x102
#define PXE_KEY_RIGHT        0x103
#define PXE_KEY_HOME         0x104
#define PXE_KEY_END          0x105
#define PXE_KEY_PGUP         0x106
#define PXE_KEY_PGDN         0x107
#define PXE_KEY_DELETE       0x108

/* Canvas pixel format */
#define PXE_CANVAS_PAL8      1u  /* 1 byte/pixel: 256-color classic palette */
#define PXE_CANVAS_RGB32     2u  /* 4 bytes/pixel: 0x00RRGGBB truecolor */

/* Window creation flags */
#define PXE_WIN_DEFAULT      0u
#define PXE_WIN_RESIZABLE    (1u << 0)
#define PXE_WIN_NO_CLOSE     (1u << 1)
#define PXE_WIN_CENTERED     (1u << 2)

/* Window event types */
#define PXE_EV_NONE          0u
#define PXE_EV_REDRAW        1u
#define PXE_EV_CLOSE         2u
#define PXE_EV_RESIZE        3u
#define PXE_EV_ACTIVATE      4u
#define PXE_EV_MOUSE_DOWN    5u
#define PXE_EV_MOUSE_UP      6u
#define PXE_EV_MOUSE_MOVE    7u
#define PXE_EV_KEY_DOWN      8u
#define PXE_EV_MENU          9u

/* Mouse button masks */
#define PXE_BTN_LEFT         (1u << 0)
#define PXE_BTN_RIGHT        (1u << 1)
#define PXE_BTN_MIDDLE       (1u << 2)

/* Keyboard modifier masks (ev.modifiers) */
#define PXE_MOD_CTRL         (1u << 0)

typedef struct {
    uint32_t type;       /* PXE_EV_* */
    int32_t  win_id;     /* Target window handle */
    int32_t  x;          /* Client-relative X */
    int32_t  y;          /* Client-relative Y */
    int32_t  w;          /* New client width (for PXE_EV_RESIZE) */
    int32_t  h;          /* New client height (for PXE_EV_RESIZE) */
    uint32_t buttons;    /* PXE_BTN_* mask */
    int32_t  key;        /* Keycode for PXE_EV_KEY_DOWN */
    uint32_t modifiers;  /* PXE_MOD_* mask */
    int32_t  arg0;       /* Menu index for PXE_EV_MENU */
    int32_t  arg1;       /* Menu item index for PXE_EV_MENU */
    char     text[64];   /* Optional payload (e.g. argv[1] on PXE_EV_ACTIVATE) */
} pxe_event_t;

typedef struct {
    int32_t  x;          /* Screen X (-1 = auto cascade / center) */
    int32_t  y;          /* Screen Y (-1 = auto cascade / center) */
    int32_t  w;          /* Client width in pixels */
    int32_t  h;          /* Client height in pixels */
    int32_t  min_w;      /* Minimum client width */
    int32_t  min_h;      /* Minimum client height */
    uint32_t format;     /* PXE_CANVAS_PAL8 or PXE_CANVAS_RGB32 */
    uint32_t flags;      /* PXE_WIN_* */
    char     title[48];  /* Initial window title */
} pxe_win_desc_t;

typedef struct {
    void*    pixels;     /* Direct pointer to pixel buffer (shared in Ring 3, direct in Ring 0) */
    int32_t  width;      /* Current client width */
    int32_t  height;     /* Current client height */
    int32_t  stride;     /* Row stride in pixels (for PAL8 = bytes, for RGB32 = pixels, pitch = stride*4) */
    int32_t  pitch;      /* Row pitch in bytes */
    uint32_t format;     /* PXE_CANVAS_PAL8 or PXE_CANVAS_RGB32 */
} pxe_win_surface_t;

#define PXE_MAX_MENUS       4
#define PXE_MAX_MENU_ITEMS  8

typedef struct {
    char     label[24];
    uint8_t  enabled;    /* 1 = clickable, 0 = grayed out */
    uint8_t  reserved[7];
} pxe_menu_item_t;

typedef struct {
    char            title[16];
    int32_t         item_count;
    pxe_menu_item_t items[PXE_MAX_MENU_ITEMS];
} pxe_menu_t;

typedef struct {
    int32_t    menu_count;
    pxe_menu_t menus[PXE_MAX_MENUS];
} pxe_menu_bar_t;

/* Real-time clock structure */
typedef struct {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint8_t  reserved;
} pxe_rtc_time_t;

/* Basic non-privileged system summary */
/* CPU feature flags reported in pxe_basic_sysinfo_t.cpu_flags */
#define PXE_CPU_SSE          (1u << 0)
#define PXE_CPU_SSE2         (1u << 1)
#define PXE_CPU_SSE3         (1u << 2)
#define PXE_CPU_SSSE3        (1u << 3)
#define PXE_CPU_SSE41        (1u << 4)
#define PXE_CPU_SSE42        (1u << 5)
#define PXE_CPU_AVX          (1u << 6)
#define PXE_CPU_AVX2         (1u << 7)
#define PXE_CPU_NX           (1u << 8)
#define PXE_CPU_PSE          (1u << 9)
#define PXE_CPU_PAE          (1u << 10)
#define PXE_CPU_APIC         (1u << 11)
#define PXE_CPU_X2APIC       (1u << 12)
#define PXE_CPU_TSC          (1u << 13)
#define PXE_CPU_RDRAND       (1u << 14)

typedef struct {
    char     os_name[32];
    char     os_build[24];
    char     os_codename[24];
    char     cpu_model[64];
    uint32_t cpu_freq_mhz;
    uint32_t total_ram_mb;
    uint32_t usable_ram_mb;
    uint32_t free_ram_mb;
    int32_t  screen_w;
    int32_t  screen_h;
    int32_t  screen_bpp;
    int32_t  boot_mode;       /* 0=Live, 1=Installer, 2=Local */
    char     system_drive;    /* 'C'..'Z' or 0 */
    uint8_t  desktop_pattern; /* 0..5 */
    uint8_t  mouse_speed;     /* 1..5 */
    uint8_t  volume;          /* 0..100 */
    char     gfx_backend[48];
    char     audio_backend[48];
    /* Extended Phase 3 fields (appended for ABI backward compatibility) */
    char     cdrom_drive;       /* 'C'..'Z' or 0 */
    uint8_t  wallpaper_active;  /* 1 if custom wallpaper active */
    uint8_t  wallpaper_mode;    /* 0=Center, 1=Tile, 2=Stretch, 3=Fill */
    uint8_t  settings_writable; /* 1 if persistent .pse write supported */
    uint8_t  has_audio_hw;      /* 1 if real audio device active */
    uint8_t  audio_muted;       /* 1 if audio muted */
    uint8_t  gfx_is_2d_hw;      /* 1 if hardware 2D accel active */
    uint8_t  gfx_has_hw_cursor; /* 1 if hardware cursor supported */
    uint16_t mode_support_mask; /* bit (res_idx*4 + bpp_idx): 640/800/1024/1280 x 32/24/16/8 */
    uint16_t _ext_pad;
    uint32_t cpu_flags;         /* PXE_CPU_* bitmask */
    uint64_t vram_bytes;
    char     cpu_vendor[16];
    char     gfx_status[48];
    char     gfx_fallback[48];
    char     audio_probe[48];
} pxe_basic_sysinfo_t;

/* Display mode & capability query ABI */
#define PXE_DISP_BPP_8              (1u << 0)
#define PXE_DISP_BPP_16             (1u << 1)
#define PXE_DISP_BPP_24             (1u << 2)
#define PXE_DISP_BPP_32             (1u << 3)
#define PXE_DISP_BPP_ALL            (PXE_DISP_BPP_8 | PXE_DISP_BPP_16 | PXE_DISP_BPP_24 | PXE_DISP_BPP_32)

#define PXE_DISP_MAX_MODES          48u
#define PXE_DISP_FLAG_DRIVER_ACTIVE (1u << 0)
#define PXE_DISP_FLAG_HW_2D         (1u << 1)
#define PXE_DISP_FLAG_UNLIMITED_RES (1u << 2)

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  bpp_mask;
    uint8_t  preferred;
    uint16_t reserved;
} pxe_display_mode_t;

typedef struct {
    uint32_t           struct_size;
    uint32_t           flags;
    int32_t            current_w;
    int32_t            current_h;
    int32_t            current_bpp;
    int32_t            min_w;
    int32_t            min_h;
    int32_t            max_w;
    int32_t            max_h;
    uint32_t           bpp_mask;
    uint64_t           vram_bytes;
    int32_t            mode_count;
    int32_t            reserved;
    char               driver_name[48];
    char               driver_status[48];
    pxe_display_mode_t modes[PXE_DISP_MAX_MODES];
} pxe_display_caps_t;

/* System settings control commands (requires PXE_FLAG_SYS_SETTINGS in Ring 3) */
#define PXE_CFG_SET_RESOLUTION     1
#define PXE_CFG_SET_PATTERN        2
#define PXE_CFG_SET_WALLPAPER      3
#define PXE_CFG_CLEAR_WALLPAPER    4
#define PXE_CFG_SET_MOUSE_SPEED    5
#define PXE_CFG_SAVE_SETTINGS      6
#define PXE_CFG_APPLY_RESOLUTION   7
#define PXE_CFG_SET_WALLPAPER_MODE 8

/* Filesystem directory & drive structures */
typedef struct {
    char     name[128];
    uint8_t  is_dir;
    uint8_t  reserved[7];
    uint64_t size;
} pxe_dirent_t;

typedef struct {
    char     letter;
    uint8_t  present;
    uint8_t  mounted;
    uint8_t  drv_type;       /* 0=Floppy, 1=HDD, 2=CDROM, 3=USB */
    uint8_t  is_readonly;
    uint8_t  is_supported;
    uint8_t  is_free_known;
    uint8_t  reserved;
    uint64_t total_kb;
    uint64_t free_kb;
    char     fs_name[16];
    char     label[32];
} pxe_drive_info_t;

/* Audio subsystem commands & status */
#define PXE_AUD_PLAY_FILE        1
#define PXE_AUD_PAUSE            2
#define PXE_AUD_RESUME           3
#define PXE_AUD_STOP             4
#define PXE_AUD_SEEK             5
#define PXE_AUD_SET_VOLUME       6
#define PXE_AUD_TOGGLE_MUTE      7
#define PXE_AUD_GET_STATUS       8
#define PXE_AUD_FEED_PCM         9

typedef struct {
    int32_t  has_hardware;
    int32_t  state;          /* 0=Stopped, 1=Playing, 2=Paused */
    int32_t  volume;         /* 0..100 */
    int32_t  muted;
    int32_t  sample_rate;
    int32_t  channels;
    int32_t  bit_depth;
    int32_t  bitrate_kbps;
    int32_t  position_sec;
    int32_t  duration_sec;
    uint8_t  spectrum[10];   /* 0..15 */
    uint8_t  vu_left;        /* 0..255 */
    uint8_t  vu_right;       /* 0..255 */
    char     hw_name[48];
    char     title[128];
} pxe_audio_status_t;

typedef struct {
    const void*  samples;
    uint32_t     sample_count;
    int32_t      src_rate;
    int32_t      src_channels;
    int32_t      src_bits;
    int32_t      src_float;
} pxe_pcm_chunk_t;

#endif /* _PXE_ABI_H */

