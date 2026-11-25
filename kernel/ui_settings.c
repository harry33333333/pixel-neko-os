#include "settings.h"
#include "audio.h"
#include "boot_framebuffer.h"
#include "fat.h"
#include "gfx_accel.h"
#include "image_dec.h"
#include "limine.h"
#include "log.h"
#include "mouse.h"
#include "string.h"
#include "vfs.h"

#define PSE_MAGIC 0x31455350u /* "PSE1" */
#define PSE_VERSION 1u
#define PSE_WALLPAPER_PATH_MAX 256
#define PSE_CLOCK_HIDE_DATE     0x01u
#define PSE_CLOCK_HIDE_SECONDS  0x02u
#define PSE_VOLUME_VALID        0x04u
#define PSE_VOLUME_MUTED        0x08u
#define PSE_FLAG_LANG_EN        0x10u
#define PSE_BOOT_CONFIG_MAX     32768

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t sequence;
    uint16_t width;
    uint16_t height;
    uint8_t bpp;
    uint8_t mouse_speed;
    uint8_t pattern;
    uint8_t wallpaper_mode;
    uint8_t wallpaper_active;
    uint8_t reserved[3];
    char wallpaper_path[PSE_WALLPAPER_PATH_MAX];
    uint32_t checksum;
} __attribute__((packed)) pse_config_t;

extern volatile struct limine_kernel_file_request kernel_file_request;
extern int screen_w;
extern int screen_h;
extern int screen_bpp;
int g_desktop_pattern = 0;

static settings_boot_mode_t g_boot_mode = SETTINGS_BOOT_LIVE;
static char g_system_drive = 0;

char settings_get_system_drive(void) {
    return g_system_drive;
}
static char g_config_path[2][40];
static int g_active_slot = -1;
static uint32_t g_sequence = 0;
static int g_saved_width = 0;
static int g_saved_height = 0;
static int g_saved_bpp = 0;
static int g_saved_refresh_hz = 60;
static char g_wallpaper_path[PSE_WALLPAPER_PATH_MAX];
static bool g_missing_notice_logged = false;
static bool g_clock_show_date = true;
static bool g_clock_show_seconds = true;
static bool g_lang_zh = true;

static unsigned int copy_text(char* target, unsigned int capacity,
                              const char* source) {
    unsigned int index = 0;
    if (!target || capacity == 0) return 0;
    while (source && source[index] && index + 1 < capacity) {
        target[index] = source[index];
        index++;
    }
    target[index] = '\0';
    return index;
}

static void append_text(char* target, unsigned int capacity,
                        const char* source) {
    unsigned int used = 0;
    if (!target || !source || capacity == 0) return;
    while (target[used] && used + 1 < capacity) used++;
    copy_text(target + used, capacity - used, source);
}

static void make_drive_path(char* output, unsigned int capacity, char drive,
                            const char* suffix) {
    if (!output || capacity < 4) return;
    output[0] = drive;
    output[1] = ':';
    output[2] = '\0';
    append_text(output, capacity, suffix);
}

static bool file_exists(const char* path) {
    vfs_dirent_t entry;
    return path && vfs_find_file(path, &entry) == VFS_OK && !entry.is_dir;
}

static bool path_has_file(char drive, const char* suffix) {
    char path[96];
    make_drive_path(path, sizeof(path), drive, suffix);
    return file_exists(path);
}

static bool boot_disk_matches(blkdev_t* dev, uint32_t disk_id) {
    if (!dev || disk_id == 0) return true;
    unsigned char mbr[512];
    if (block_read(dev, 0, 1, mbr) != 0) return false;
    uint32_t signature = (uint32_t)mbr[440] |
                         ((uint32_t)mbr[441] << 8) |
                         ((uint32_t)mbr[442] << 16) |
                         ((uint32_t)mbr[443] << 24);
    return signature == disk_id;
}

static bool find_system_partition(void) {
    if (g_boot_mode != SETTINGS_BOOT_LOCAL) return false;
    if (kernel_file_request.response &&
        kernel_file_request.response->kernel_file &&
        kernel_file_request.response->kernel_file->media_type ==
            LIMINE_MEDIA_TYPE_OPTICAL) {
        log_printf("[SETTINGS] Local mode rejected on optical boot media\n");
        return false;
    }

    const struct limine_file* boot_file =
        kernel_file_request.response ?
            kernel_file_request.response->kernel_file : NULL;
    uint32_t boot_disk_id = boot_file ? boot_file->mbr_disk_id : 0;
    for (int relaxed = 0; relaxed < 2; relaxed++) {
        for (char letter = 'C'; letter <= 'Z'; letter++) {
            vfs_drive_t* drive = vfs_get_drive(letter);
            if (!drive || !drive->present || !drive->mounted ||
                drive->is_readonly ||
                (drive->drv_type != VFS_DRV_HDD &&
                 drive->drv_type != VFS_DRV_USB) ||
                !drive->dev || !drive->is_supported) {
                continue;
            }
            if (drive->fs_type != PART_FS_FAT12 &&
                drive->fs_type != PART_FS_FAT16 &&
                drive->fs_type != PART_FS_FAT32 &&
                drive->fs_type != PART_FS_EXFAT) {
                continue;
            }
            if (relaxed == 0 &&
                !boot_disk_matches(drive->dev, boot_disk_id)) {
                continue;
            }
            /*
             * PAYLOAD.TXT is optional for systems installed by older
             * releases.  The kernel plus the system app directory are the
             * stable identity of an installed Pixel Neko volume.
             */
            if (path_has_file(letter, "\\boot\\kernel.elf") &&
                (path_has_file(letter, "\\APPS\\files.pxe") ||
                 path_has_file(letter, "\\system\\PAYLOAD.TXT"))) {
                g_system_drive = letter;
                make_drive_path(g_config_path[0], sizeof(g_config_path[0]),
                                letter, "\\system\\PIXEL.pse");
                make_drive_path(g_config_path[1], sizeof(g_config_path[1]),
                                letter, "\\system\\PIXEL2.pse");
                log_printf("[SETTINGS] system partition=%c (%s): config=%s\n",
                           letter, relaxed ? "fallback scan" : "boot disk",
                           g_config_path[0]);
                return true;
            }
        }
    }
    return false;
}

static uint32_t pse_crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t index = 0; index < length; index++) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static bool text_has_terminator(const char* text, size_t capacity) {
    for (size_t index = 0; index < capacity; index++) {
        if (text[index] == '\0') return true;
    }
    return false;
}

static bool pse_valid(const pse_config_t* config) {
    if (!config || config->magic != PSE_MAGIC ||
        config->version != PSE_VERSION ||
        config->size != sizeof(*config) ||
        config->sequence == 0 ||
        config->mouse_speed < 1 || config->mouse_speed > 5 ||
        config->pattern > 5 || config->wallpaper_mode > WP_MODE_CENTER ||
        config->wallpaper_active > 1 ||
        !text_has_terminator(config->wallpaper_path,
                             sizeof(config->wallpaper_path))) {
        return false;
    }
    uint32_t expected = pse_crc32(
        (const uint8_t*)config, sizeof(*config) - sizeof(config->checksum));
    return expected == config->checksum;
}

static bool read_slot(int slot, pse_config_t* config) {
    if (slot < 0 || slot > 1 || !config) return false;
    pse_config_t buffer;
    memset(&buffer, 0, sizeof(buffer));
    int read = vfs_read_file(g_config_path[slot], (unsigned char*)&buffer,
                             sizeof(buffer));
    if (read != (int)sizeof(buffer) || !pse_valid(&buffer)) return false;
    *config = buffer;
    return true;
}

static bool pse_resolution_known(int width, int height, int bpp) {
    if (width < GFX_MIN_WIDTH || height < GFX_MIN_HEIGHT) return false;
    if (width > 16384 || height > 16384) return false;
    return (bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32);
}

static char* append_unsigned(char* out, unsigned int value) {
    char digits[12];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value && count < (int)sizeof(digits));
    while (count > 0) *out++ = digits[--count];
    return out;
}

static void format_boot_resolution(char* out, size_t capacity,
                                   int width, int height, int bpp) {
    if (!out || capacity < 12) return;
    char* cursor = out;
    cursor = append_unsigned(cursor, (unsigned int)width);
    *cursor++ = 'x';
    cursor = append_unsigned(cursor, (unsigned int)height);
    *cursor++ = 'x';
    cursor = append_unsigned(cursor, (unsigned int)bpp);
    *cursor = '\0';
}

static bool replace_first_resolution(char* text, int length,
                                     size_t capacity,
                                     const char* resolution,
                                     int* out_length) {
    if (!text || !resolution || length <= 0 || (size_t)length >= capacity) {
        return false;
    }
    size_t new_length = strlen(resolution);
    int cursor = 0;
    while (cursor < length) {
        int line_start = cursor;
        while (line_start < length &&
               (text[line_start] == ' ' || text[line_start] == '\t')) {
            line_start++;
        }
        static const char key[] = "resolution:";
        bool match = line_start + (int)sizeof(key) - 1 <= length;
        for (size_t index = 0; match && index < sizeof(key) - 1; index++) {
            if (text[line_start + (int)index] != key[index]) match = false;
        }
        int line_end = cursor;
        while (line_end < length && text[line_end] != '\r' &&
               text[line_end] != '\n') {
            line_end++;
        }
        if (match) {
            int value_start = line_start + (int)sizeof(key) - 1;
            while (value_start < line_end &&
                   (text[value_start] == ' ' || text[value_start] == '\t')) {
                value_start++;
            }
            int old_length = line_end - value_start;
            int delta = (int)new_length - old_length;
            if ((size_t)(length + delta) >= capacity) return false;
            memmove(text + line_end + delta, text + line_end,
                    (size_t)(length - line_end + 1));
            memcpy(text + value_start, resolution, new_length);
            if (out_length) *out_length = length + delta;
            return true;
        }
        cursor = line_end;
        while (cursor < length &&
               (text[cursor] == '\r' || text[cursor] == '\n')) {
            cursor++;
        }
    }
    return false;
}

bool settings_stage_boot_resolution(int width, int height, int bpp) {
    if (!pse_resolution_known(width, height, bpp) ||
        settings_get_boot_mode() != SETTINGS_BOOT_LOCAL) {
        return false;
    }
    char drive = settings_get_system_drive();
    if (!drive) return false;

    char resolution[24];
    format_boot_resolution(resolution, sizeof(resolution),
                           width, height, bpp);
    static char source[PSE_BOOT_CONFIG_MAX];
    static char updated[PSE_BOOT_CONFIG_MAX];
    static const char* const suffixes[] = {
        "\\boot\\limine.conf",
        "\\limine.conf",
        "\\boot\\limine\\limine.conf"
    };
    int updated_files = 0;
    for (size_t index = 0;
         index < sizeof(suffixes) / sizeof(suffixes[0]); index++) {
        char path[64];
        path[0] = drive;
        path[1] = ':';
        strcpy(path + 2, suffixes[index]);
        memset(source, 0, sizeof(source));
        int length = vfs_read_file(path, (unsigned char*)source,
                                   sizeof(source) - 1);
        if (length <= 0) continue;
        memcpy(updated, source, (size_t)length + 1);
        int updated_length = 0;
        if (!replace_first_resolution(updated, length, sizeof(updated),
                                      resolution, &updated_length)) {
            continue;
        }
        if (vfs_write_file(path, (const unsigned char*)updated,
                           (unsigned int)updated_length) == 0) {
            updated_files++;
        }
    }
    if (updated_files == 0) {
        log_printf("[SETTINGS] Could not update a writable Limine config.\n");
        return false;
    }

    settings_set_resolution(width, height, bpp);
    if (!settings_save()) {
        log_printf("[SETTINGS] Mode config written, but .pse save failed.\n");
        return false;
    }
    (void)vfs_sync_usb_drives();
    log_printf("[SETTINGS] Staged real boot mode %dx%dx%d in %d config file(s)\n",
               width, height, bpp, updated_files);
    return true;
}

static void apply_resolution(const pse_config_t* config) {
    int old_w = screen_w;
    int old_h = screen_h;
    int old_bpp = screen_bpp;
    int old_hz = gfx_accel_get_current_refresh_hz();
    int width = config->width;
    int height = config->height;
    int bpp = config->bpp;
    int hz = (config->reserved[2] >= 24u) ? (int)config->reserved[2] : 60;

    if (!pse_resolution_known(width, height, bpp)) {
        log_printf("[SETTINGS] invalid resolution %dx%d@%d; using %dx%d@%d\n",
                   width, height, bpp, old_w, old_h, old_bpp);
        goto fallback;
    }
    if (width == old_w && height == old_h && bpp == old_bpp && hz == old_hz) {
        g_saved_width = width;
        g_saved_height = height;
        g_saved_bpp = bpp;
        g_saved_refresh_hz = hz;
        return;
    }
    if (!gfx_accel_can_change_mode_ex(width, height, bpp, hz)) {
        hz = 60;
        if (!gfx_accel_can_change_mode_ex(width, height, bpp, hz)) {
            log_printf("[SETTINGS] unsupported resolution %dx%d@%d; "
                       "keeping %dx%d@%d\n",
                       width, height, bpp, old_w, old_h, old_bpp);
            goto fallback;
        }
    }
    if (!gfx_accel_try_set_mode_ex(width, height, bpp, hz)) {
        log_printf("[SETTINGS] failed resolution %dx%d@%d (%dHz); "
                   "rolling back to %dx%d@%d\n",
                   width, height, bpp, hz, old_w, old_h, old_bpp);
        gfx_accel_try_set_mode_ex(old_w, old_h, old_bpp, old_hz);
        goto fallback;
    }
    g_saved_width = width;
    g_saved_height = height;
    g_saved_bpp = bpp;
    g_saved_refresh_hz = gfx_accel_get_current_refresh_hz();
    log_printf("[SETTINGS] restored resolution %dx%d@%d (%dHz)\n",
               width, height, bpp, g_saved_refresh_hz);
    return;

fallback:
    g_saved_width = old_w;
    g_saved_height = old_h;
    g_saved_bpp = old_bpp;
    g_saved_refresh_hz = old_hz;
}

static void resolve_wallpaper_path(const char* stored, char* output,
                                   unsigned int capacity) {
    if (!output || capacity == 0) return;
    output[0] = '\0';
    if (!stored || !stored[0]) return;
    if (stored[1] == ':') {
        copy_text(output, capacity, stored);
    } else if (g_system_drive) {
        make_drive_path(output, capacity, g_system_drive, stored);
    } else {
        copy_text(output, capacity, stored);
    }
}

static bool apply_wallpaper(const pse_config_t* config) {
    wallpaper_clear();
    if (!config->wallpaper_active || !config->wallpaper_path[0]) return true;

    char path[PSE_WALLPAPER_PATH_MAX + 4];
    resolve_wallpaper_path(config->wallpaper_path, path, sizeof(path));
    if (!file_exists(path)) {
        log_printf("[SETTINGS] wallpaper missing: %s; using fallback\n", path);
        return false;
    }

    image_t image;
    memset(&image, 0, sizeof(image));
    char error[64];
    error[0] = '\0';
    if (image_load_from_file(path, &image, error, sizeof(error)) != 0 ||
        wallpaper_set_image(&image, (wallpaper_mode_t)config->wallpaper_mode) !=
            0) {
        log_printf("[SETTINGS] wallpaper load failed: %s; using fallback\n",
                   error[0] ? error : path);
        image_free(&image);
        wallpaper_clear();
        return false;
    }
    image_free(&image);
    copy_text(g_wallpaper_path, sizeof(g_wallpaper_path),
              config->wallpaper_path);
    return true;
}

void settings_set_boot_mode(settings_boot_mode_t mode) {
    g_boot_mode = mode;
}

settings_boot_mode_t settings_get_boot_mode(void) {
    return g_boot_mode;
}

void settings_init(void) {
    wallpaper_init();
    g_system_drive = 0;
    g_config_path[0][0] = '\0';
    g_config_path[1][0] = '\0';
    g_active_slot = -1;
    g_sequence = 0;
    g_saved_width = 0;
    g_saved_height = 0;
    g_saved_bpp = 0;
    g_wallpaper_path[0] = '\0';
    g_missing_notice_logged = false;
    g_clock_show_date = true;
    g_clock_show_seconds = true;
    g_lang_zh = true;

    if (g_boot_mode != SETTINGS_BOOT_LOCAL) {
        log_printf("[SETTINGS] persistent config disabled in %s mode\n",
                   g_boot_mode == SETTINGS_BOOT_INSTALLER ?
                       "installer" : "live");
        return;
    }
    if (!find_system_partition()) {
        log_printf("[SETTINGS] local boot has no writable installed system "
                   "partition; persistence disabled\n");
        return;
    }

    pse_config_t records[2];
    bool valid0 = read_slot(0, &records[0]);
    bool valid1 = read_slot(1, &records[1]);
    int sel = -1;
    if (valid0 && valid1) {
        sel = (records[1].sequence > records[0].sequence) ? 1 : 0;
    } else if (valid0) {
        sel = 0;
    } else if (valid1) {
        sel = 1;
    }
    if (sel >= 0) {
        g_lang_zh = (records[sel].reserved[0] & PSE_FLAG_LANG_EN) == 0;
    }
}

bool settings_storage_available(void) {
    return g_boot_mode == SETTINGS_BOOT_LOCAL && g_system_drive != 0 &&
           g_config_path[0][0] != '\0';
}

bool settings_load(void) {
    g_saved_width = screen_w;
    g_saved_height = screen_h;
    g_saved_bpp = screen_bpp;
    if (!settings_storage_available()) {
        return false;
    }

    pse_config_t records[2];
    bool valid[2];
    valid[0] = read_slot(0, &records[0]);
    valid[1] = read_slot(1, &records[1]);
    int selected = -1;
    if (valid[0] && valid[1]) {
        selected = records[1].sequence > records[0].sequence ? 1 : 0;
    } else if (valid[0]) {
        selected = 0;
    } else if (valid[1]) {
        selected = 1;
    }

    if (selected < 0) {
        if (!g_missing_notice_logged &&
            (file_exists(g_config_path[0]) || file_exists(g_config_path[1]))) {
            log_printf("[SETTINGS] no valid .pse record; using defaults\n");
            g_missing_notice_logged = true;
        }
        return false;
    }

    g_active_slot = selected;
    g_sequence = records[selected].sequence;
    if ((valid[0] != valid[1]) && selected >= 0) {
        log_printf("[SETTINGS] recovered valid .pse slot %d\n", selected + 1);
    }
    mouse_set_speed(records[selected].mouse_speed);
    g_desktop_pattern = records[selected].pattern;
    g_clock_show_date =
        (records[selected].reserved[0] & PSE_CLOCK_HIDE_DATE) == 0;
    g_clock_show_seconds =
        (records[selected].reserved[0] & PSE_CLOCK_HIDE_SECONDS) == 0;
    g_lang_zh =
        (records[selected].reserved[0] & PSE_FLAG_LANG_EN) == 0;
    if (records[selected].reserved[0] & PSE_VOLUME_VALID) {
        int vol = (int)records[selected].reserved[1];
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        audio_set_volume(vol);
        audio_set_muted((records[selected].reserved[0] & PSE_VOLUME_MUTED) != 0);
    }
    apply_resolution(&records[selected]);
    if (!apply_wallpaper(&records[selected])) {
        g_wallpaper_path[0] = '\0';
    }
    log_printf("[SETTINGS] loaded slot=%d seq=%u mouse=%d wallpaper=%s\n",
               selected + 1, g_sequence, mouse_get_speed(),
               wallpaper_is_active() ? "on" : "fallback");
    return true;
}

void settings_set_mouse_speed(int speed) {
    if (speed >= 1 && speed <= 5) mouse_set_speed(speed);
}

void settings_set_resolution(int width, int height, int bpp) {
    g_saved_width = width;
    g_saved_height = height;
    g_saved_bpp = bpp;
    g_saved_refresh_hz = gfx_accel_get_current_refresh_hz();
}

void settings_set_resolution_ex(int width, int height, int bpp, int refresh_hz) {
    g_saved_width = width;
    g_saved_height = height;
    g_saved_bpp = bpp;
    g_saved_refresh_hz = (refresh_hz >= 24 && refresh_hz <= 255) ? refresh_hz : 60;
}

int settings_get_refresh_hz(void) {
    return g_saved_refresh_hz > 0 ? g_saved_refresh_hz : 60;
}

void settings_set_pattern(int pattern) {
    if (pattern >= 0 && pattern <= 5) g_desktop_pattern = pattern;
}

void settings_set_clock_options(bool show_date, bool show_seconds) {
    g_clock_show_date = show_date;
    g_clock_show_seconds = show_seconds;
}

bool settings_get_clock_show_date(void) {
    return g_clock_show_date;
}

bool settings_get_clock_show_seconds(void) {
    return g_clock_show_seconds;
}

void settings_set_language_zh(bool zh) {
    g_lang_zh = zh;
}

bool settings_is_language_zh(void) {
    return g_lang_zh;
}

void settings_restore_default_wallpaper(void) {
    wallpaper_clear();
    g_wallpaper_path[0] = '\0';
    g_desktop_pattern = 0;
}

void settings_set_wallpaper(const char* path, wallpaper_mode_t mode) {
    wallpaper_set_mode(mode);
    g_wallpaper_path[0] = '\0';
    if (!path || !path[0]) return;
    if (g_system_drive && path[0] && path[1] == ':' &&
        (path[0] == g_system_drive ||
         (path[0] >= 'a' && path[0] <= 'z' &&
          path[0] - 32 == g_system_drive))) {
        copy_text(g_wallpaper_path, sizeof(g_wallpaper_path), path + 2);
    } else {
        copy_text(g_wallpaper_path, sizeof(g_wallpaper_path), path);
    }
}

void settings_set_wallpaper_mode(wallpaper_mode_t mode) {
    wallpaper_set_mode(mode);
}

void settings_clear_wallpaper(void) {
    wallpaper_clear();
    g_wallpaper_path[0] = '\0';
}

bool settings_save(void) {
    if (!settings_storage_available()) return false;

    int target = g_active_slot < 0 ? 0 : 1 - g_active_slot;
    pse_config_t config;
    memset(&config, 0, sizeof(config));
    config.magic = PSE_MAGIC;
    config.version = PSE_VERSION;
    config.size = sizeof(config);
    config.sequence = ++g_sequence;
    config.width = (uint16_t)g_saved_width;
    config.height = (uint16_t)g_saved_height;
    config.bpp = (uint8_t)g_saved_bpp;
    config.mouse_speed = (uint8_t)mouse_get_speed();
    config.pattern = (uint8_t)g_desktop_pattern;
    config.wallpaper_mode = (uint8_t)wallpaper_get_mode();
    config.wallpaper_active =
        (uint8_t)(wallpaper_is_active() && g_wallpaper_path[0]);
    config.reserved[0] = (uint8_t)(
        (g_clock_show_date ? 0u : PSE_CLOCK_HIDE_DATE) |
        (g_clock_show_seconds ? 0u : PSE_CLOCK_HIDE_SECONDS) |
        PSE_VOLUME_VALID |
        (audio_is_muted() ? PSE_VOLUME_MUTED : 0u) |
        (g_lang_zh ? 0u : PSE_FLAG_LANG_EN));
    config.reserved[1] = (uint8_t)audio_get_master_volume();
    config.reserved[2] = (uint8_t)((g_saved_refresh_hz >= 24 && g_saved_refresh_hz <= 255)
                                       ? g_saved_refresh_hz : 60);
    copy_text(config.wallpaper_path, sizeof(config.wallpaper_path),
              g_wallpaper_path);
    config.checksum = pse_crc32(
        (const uint8_t*)&config, sizeof(config) - sizeof(config.checksum));

    if (vfs_write_file(g_config_path[target], (const unsigned char*)&config,
                       sizeof(config)) != VFS_OK) {
        g_sequence--;
        log_printf("[SETTINGS] failed to write %s; previous slot retained\n",
                   g_config_path[target]);
        return false;
    }
    fat_flush_all();

    pse_config_t verify;
    if (!read_slot(target, &verify) ||
        verify.sequence != config.sequence) {
        g_sequence--;
        log_printf("[SETTINGS] .pse verify failed; previous slot retained\n");
        return false;
    }

    g_active_slot = target;
    log_printf("[SETTINGS] saved slot=%d seq=%u res=%dx%d@%d mouse=%d\n",
               target + 1, g_sequence, g_saved_width, g_saved_height,
               g_saved_bpp, mouse_get_speed());
    return true;
}
