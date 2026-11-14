#include "settings.h"
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
static char g_wallpaper_path[PSE_WALLPAPER_PATH_MAX];
static bool g_missing_notice_logged = false;

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
    for (char letter = 'C'; letter <= 'Z'; letter++) {
        vfs_drive_t* drive = vfs_get_drive(letter);
        if (!drive || !drive->present || !drive->mounted ||
            drive->is_readonly || drive->drv_type != VFS_DRV_HDD ||
            !drive->dev || drive->dev->is_removable ||
            !drive->is_supported) {
            continue;
        }
        if (drive->fs_type != PART_FS_FAT12 &&
            drive->fs_type != PART_FS_FAT16 &&
            drive->fs_type != PART_FS_FAT32 &&
            drive->fs_type != PART_FS_EXFAT) {
            continue;
        }
        if (!boot_disk_matches(drive->dev, boot_disk_id)) continue;
        if (path_has_file(letter, "\\boot\\kernel.elf") &&
            path_has_file(letter, "\\system\\PAYLOAD.TXT")) {
            g_system_drive = letter;
            make_drive_path(g_config_path[0], sizeof(g_config_path[0]),
                            letter, "\\system\\PIXEL.pse");
            make_drive_path(g_config_path[1], sizeof(g_config_path[1]),
                            letter, "\\system\\PIXEL2.pse");
            log_printf("[SETTINGS] system partition=%c: config=%s\n",
                       letter, g_config_path[0]);
            return true;
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
        config->pattern > 5 || config->wallpaper_mode > WP_MODE_TILE ||
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

static void apply_resolution(const pse_config_t* config) {
    int old_w = screen_w;
    int old_h = screen_h;
    int old_bpp = screen_bpp;
    int width = config->width;
    int height = config->height;
    int bpp = config->bpp;

    if (!pse_resolution_known(width, height, bpp)) {
        log_printf("[SETTINGS] invalid resolution %dx%d@%d; using %dx%d@%d\n",
                   width, height, bpp, old_w, old_h, old_bpp);
        goto fallback;
    }
    if (!gfx_accel_can_change_mode(width, height, bpp) ||
        !gfx_accel_try_set_mode(width, height, bpp)) {
        log_printf("[SETTINGS] unsupported resolution %dx%d@%d; "
                   "rolling back to %dx%d@%d\n",
                   width, height, bpp, old_w, old_h, old_bpp);
        gfx_accel_try_set_mode(old_w, old_h, old_bpp);
        goto fallback;
    }
    g_saved_width = width;
    g_saved_height = height;
    g_saved_bpp = bpp;
    log_printf("[SETTINGS] restored resolution %dx%d@%d\n",
               width, height, bpp);
    return;

fallback:
    g_saved_width = old_w;
    g_saved_height = old_h;
    g_saved_bpp = old_bpp;
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

    if (g_boot_mode != SETTINGS_BOOT_LOCAL) {
        log_printf("[SETTINGS] persistent config disabled in %s mode\n",
                   g_boot_mode == SETTINGS_BOOT_INSTALLER ?
                       "installer" : "live");
        return;
    }
    if (!find_system_partition()) {
        log_printf("[SETTINGS] local boot has no writable installed system "
                   "partition; persistence disabled\n");
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
}

void settings_set_pattern(int pattern) {
    if (pattern >= 0 && pattern <= 5) g_desktop_pattern = pattern;
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
