#include "installer.h"
#include "blkdev.h"
#include "fat.h"
#include "font.h"
#include "format.h"
#include "log.h"
#include "part.h"
#include "power.h"
#include "string.h"
#include "ui_memory.h"
#include "vfs.h"
#include "wm.h"

/*
 * The BIOS stage-one image is generated from the same Limine release as the
 * ISO. Keeping it in the kernel makes offline installation independent of a
 * host-side limine executable.
 */
#include "../boot/limine/limine-bios-hdd.h"

extern int redraw;
extern int insv;

#define INSTALL_MAX_TARGETS 16
#define INSTALL_MIN_MB 64u
#define INSTALL_FAT16_MAX_SECTORS 4194304ull
#define INSTALL_FAT32_MIN_SECTORS 65536ull
#define PAYLOAD_LIST_CAPACITY 16

typedef enum {
    STEP_SELECT = 0,
    STEP_FORMAT,
    STEP_CONFIRM,
    STEP_PROGRESS,
    STEP_COMPLETE
} install_step_t;

typedef struct {
    partition_entry_t* part;
    unsigned int total_mb;
    char fs_name[12];
    char disk_name[24];
    int disk_index;
    int partition_index;
} target_part_t;

typedef struct {
    vfs_dirent_t entries[PAYLOAD_LIST_CAPACITY];
    int count;
    int overflow;
} payload_list_t;

static install_step_t g_step = STEP_SELECT;
static int g_selected_idx;
static int g_target_count;
static target_part_t g_targets[INSTALL_MAX_TARGETS];
static target_part_t* g_target;
static part_fs_type_t g_format_type = PART_FS_FAT32;
static int g_install_error;
static int g_progress_percent;
static char g_progress_text[72];
static char g_progress_subtext[72];
static payload_list_t* g_payload_collector;

static void on_payload_write_progress(unsigned int written,
                                      unsigned int total) {
    log_printf("[P6 ] Payload bytes %u/%u\n", written, total);
}

static void text_copy(char* destination, const char* source, int capacity) {
    int index = 0;
    if (!destination || capacity <= 0) return;
    while (source && source[index] && index + 1 < capacity) {
        destination[index] = source[index];
        index++;
    }
    destination[index] = '\0';
}

static void text_append(char* destination, const char* source, int capacity) {
    int offset = 0;
    if (!destination || !source || capacity <= 0) return;
    while (destination[offset] && offset + 1 < capacity) offset++;
    while (*source && offset + 1 < capacity) {
        destination[offset++] = *source++;
    }
    destination[offset] = '\0';
}

static void format_uint(unsigned int value, char* buffer) {
    char reversed[16];
    int count = 0;
    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }
    while (value && count < (int)sizeof(reversed)) {
        reversed[count++] = (char)('0' + value % 10u);
        value /= 10u;
    }
    int out = 0;
    while (count > 0) buffer[out++] = reversed[--count];
    buffer[out] = '\0';
}

static uint32_t read_le32(const unsigned char* data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint64_t read_le64(const unsigned char* data) {
    return (uint64_t)read_le32(data) |
           ((uint64_t)read_le32(data + 4) << 32);
}

static void put_be16(unsigned char* data, uint16_t value) {
    data[0] = (unsigned char)(value >> 8);
    data[1] = (unsigned char)value;
}

static void put_be64(unsigned char* data, uint64_t value) {
    for (int index = 0; index < 8; index++) {
        data[index] = (unsigned char)(value >> (56 - index * 8));
    }
}

static void put_le32(unsigned char* data, uint32_t value) {
    data[0] = (unsigned char)value;
    data[1] = (unsigned char)(value >> 8);
    data[2] = (unsigned char)(value >> 16);
    data[3] = (unsigned char)(value >> 24);
}

static uint32_t crc32_update(uint32_t crc, const unsigned char* data,
                             size_t length) {
    for (size_t index = 0; index < length; index++) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

static int target_supports_format(const target_part_t* target,
                                  part_fs_type_t type) {
    if (!target || !target->part) return 0;
    if (type == PART_FS_FAT32) {
        return target->part->total_sectors >= INSTALL_FAT32_MIN_SECTORS;
    }
    if (type == PART_FS_FAT16) {
        return target->part->total_sectors <= INSTALL_FAT16_MAX_SECTORS;
    }
    return 0;
}

static void scan_targets(void) {
    int disk_ids[INSTALL_MAX_TARGETS];
    int disk_count = 0;
    g_target_count = 0;

    for (int index = 0;
         index < g_partition_count && g_target_count < INSTALL_MAX_TARGETS;
         index++) {
        partition_entry_t* part = &g_partitions[index];
        blkdev_t* dev = part->dev;
        if (!dev || dev->is_readonly || dev->sector_size != 512 ||
            (dev->type != BLK_TYPE_AHCI && dev->type != BLK_TYPE_ATA)) {
            continue;
        }
        unsigned int total_mb =
            (unsigned int)(part->total_sectors / 2048ull);
        if (total_mb < INSTALL_MIN_MB) continue;

        int disk_index = -1;
        for (int item = 0; item < disk_count; item++) {
            if (disk_ids[item] == dev->id) {
                disk_index = item;
                break;
            }
        }
        if (disk_index < 0) {
            disk_index = disk_count;
            disk_ids[disk_count++] = dev->id;
        }

        target_part_t* target = &g_targets[g_target_count];
        memset(target, 0, sizeof(*target));
        target->part = part;
        target->total_mb = total_mb;
        target->disk_index = disk_index + 1;
        target->partition_index = 0;
        for (int prior = 0; prior <= index; prior++) {
            if (g_partitions[prior].dev == dev) target->partition_index++;
        }
        text_copy(target->fs_name,
                  part->is_supported ? vfs_get_fs_name(part->fs_type) :
                                       "Unknown",
                  sizeof(target->fs_name));
        text_copy(target->disk_name, dev->name, sizeof(target->disk_name));
        g_target_count++;
    }

    if (g_target_count > 0) {
        if (g_selected_idx < 0 || g_selected_idx >= g_target_count) {
            g_selected_idx = 0;
        }
    } else {
        g_selected_idx = -1;
    }
    log_printf("[P6 ] Installer scan found %d writable AHCI/IDE partition(s).\n",
               g_target_count);
}

void installer_init(void) {
    g_step = STEP_SELECT;
    g_selected_idx = 0;
    g_target = NULL;
    g_install_error = 0;
    g_format_type = PART_FS_FAT32;
    g_progress_percent = 0;
    text_copy(g_progress_text, "Preparing installation...",
              sizeof(g_progress_text));
    g_progress_subtext[0] = '\0';
    scan_targets();
}

void installer_open(void) {
    installer_init();
    insv = 1;
    redraw = 1;
}

static void update_progress_ui(int percent, const char* text,
                               const char* subtext) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    g_progress_percent = percent;
    text_copy(g_progress_text, text, sizeof(g_progress_text));
    text_copy(g_progress_subtext, subtext ? subtext : "",
              sizeof(g_progress_subtext));
    redraw = 1;
    wm_draw(backbuffer);
    copy_fullscreen_to_lfb(backbuffer);
}

static void draw_button(unsigned char* surface, int x, int y, int width,
                        int height, const char* label, int enabled) {
    int face = enabled ? 7 : 8;
    int text = enabled ? 0 : 7;
    for (int py = y; py < y + height; py++) {
        for (int px = x; px < x + width; px++) {
            PUT_PIXEL(surface, px, py, face);
        }
    }
    for (int px = x; px < x + width; px++) {
        PUT_PIXEL(surface, px, y, enabled ? 15 : 7);
        PUT_PIXEL(surface, px, y + height - 1, 0);
    }
    for (int py = y; py < y + height; py++) {
        PUT_PIXEL(surface, x, py, enabled ? 15 : 7);
        PUT_PIXEL(surface, x + width - 1, py, 0);
    }
    int length = 0;
    while (label[length]) length++;
    font_draw_text(surface, x + (width - length * 8) / 2,
                   y + height / 2 - 4, label, text);
}

static void draw_inset(unsigned char* surface, int x, int y, int width,
                       int height) {
    for (int py = y; py < y + height; py++) {
        for (int px = x; px < x + width; px++) {
            PUT_PIXEL(surface, px, py, 15);
        }
    }
    for (int px = x; px < x + width; px++) {
        PUT_PIXEL(surface, px, y, 8);
        PUT_PIXEL(surface, px, y + height - 1, 7);
    }
    for (int py = y; py < y + height; py++) {
        PUT_PIXEL(surface, x, py, 8);
        PUT_PIXEL(surface, x + width - 1, py, 7);
    }
}

static void target_description(char* line, int capacity,
                               const target_part_t* target, int selected) {
    char number[16];
    line[0] = '\0';
    if (selected) text_append(line, "> ", capacity);
    text_append(line, "Disk ", capacity);
    format_uint((unsigned int)target->disk_index, number);
    text_append(line, number, capacity);
    text_append(line, " P", capacity);
    format_uint((unsigned int)target->partition_index, number);
    text_append(line, number, capacity);
    text_append(line, "  ", capacity);
    format_uint(target->total_mb, number);
    text_append(line, number, capacity);
    text_append(line, " MB  [", capacity);
    text_append(line, target->fs_name, capacity);
    text_append(line, "]", capacity);
}

static int next_button_y(int wy, int wh) {
    return wy + wh - 32;
}

static int next_button_x(int wx, int ww) {
    return wx + ww - 92;
}

void installer_draw(unsigned char* surface, int wx, int wy,
                    int ww, int wh) {
    for (int py = wy + 20; py < wy + wh; py++) {
        for (int px = wx; px < wx + ww; px++) {
            PUT_PIXEL(surface, px, py, 7);
        }
    }

    if (g_step == STEP_SELECT) {
        font_draw_text(surface, wx + 14, wy + 28,
                       "Install Pixel Neko OS to an existing partition", 0);
        font_draw_text(surface, wx + 14, wy + 46,
                       "Choose a target. The partition table is not changed.", 8);

        int list_x = wx + 14;
        int list_y = wy + 66;
        int list_width = ww - 28;
        int visible = g_target_count > 8 ? 8 : g_target_count;
        int list_height = visible > 0 ? visible * 22 + 12 : 64;
        draw_inset(surface, list_x, list_y, list_width, list_height);
        if (g_target_count == 0) {
            font_draw_text(surface, list_x + 12, list_y + 16,
                           "No writable AHCI/IDE partition of 64 MB or larger.", 12);
            font_draw_text(surface, list_x + 12, list_y + 36,
                           "Connect a disk with an existing GPT/MBR partition.", 8);
        } else {
            for (int item = 0; item < visible; item++) {
                int row_y = list_y + 6 + item * 22;
                if (item == g_selected_idx) {
                    for (int py = row_y - 2; py < row_y + 18; py++) {
                        for (int px = list_x + 4;
                             px < list_x + list_width - 4; px++) {
                            PUT_PIXEL(surface, px, py, 1);
                        }
                    }
                }
                char line[96];
                target_description(line, sizeof(line), &g_targets[item],
                                   item == g_selected_idx);
                font_draw_text(surface, list_x + 10, row_y,
                               line, item == g_selected_idx ? 15 : 0);
            }
        }

        if (g_target_count > 0) {
            char details[96];
            details[0] = '\0';
            text_append(details, "Device: ", sizeof(details));
            text_append(details, g_targets[g_selected_idx].disk_name,
                        sizeof(details));
            font_draw_text(surface, wx + 14, list_y + list_height + 16,
                           details, 0);
            font_draw_text(surface, wx + 14, list_y + list_height + 34,
                           "GPT/MBR geometry and all non-target partitions are preserved.", 8);
        }

        draw_button(surface, next_button_x(wx, ww),
                    next_button_y(wy, wh), 78, 23, "Next >",
                    g_target_count > 0);
        draw_button(surface, wx + 14, next_button_y(wy, wh),
                    78, 23, "Cancel", 1);
    } else if (g_step == STEP_FORMAT) {
        font_draw_text(surface, wx + 14, wy + 28,
                       "Choose the destination filesystem", 0);
        font_draw_text(surface, wx + 14, wy + 48,
                       "Formatting removes every file on the selected partition.", 12);

        int box_x = wx + 14;
        int box_width = ww - 28;
        int fat32_y = wy + 76;
        int fat16_y = wy + 130;
        draw_inset(surface, box_x, fat32_y, box_width, 42);
        draw_inset(surface, box_x, fat16_y, box_width, 42);
        if (g_format_type == PART_FS_FAT32) {
            for (int py = fat32_y + 2; py < fat32_y + 40; py++) {
                for (int px = box_x + 2; px < box_x + box_width - 2; px++) {
                    PUT_PIXEL(surface, px, py, 1);
                }
            }
        } else {
            for (int py = fat16_y + 2; py < fat16_y + 40; py++) {
                for (int px = box_x + 2; px < box_x + box_width - 2; px++) {
                    PUT_PIXEL(surface, px, py, 1);
                }
            }
        }
        font_draw_text(surface, box_x + 12, fat32_y + 8,
                       "FAT32 (recommended for large and modern disks)",
                       g_format_type == PART_FS_FAT32 ? 15 : 0);
        font_draw_text(surface, box_x + 12, fat32_y + 24,
                       "Supports this target",
                       g_format_type == PART_FS_FAT32 ? 15 : 8);
        font_draw_text(surface, box_x + 12, fat16_y + 8,
                       "FAT16 (maximum target size approximately 2 GB)",
                       g_format_type == PART_FS_FAT16 ? 15 : 0);
        font_draw_text(surface, box_x + 12, fat16_y + 24,
                       target_supports_format(g_target, PART_FS_FAT16)
                           ? "Compatible legacy filesystem" :
                             "Not available for this partition size",
                       g_format_type == PART_FS_FAT16 ? 15 : 8);

        draw_button(surface, next_button_x(wx, ww),
                    next_button_y(wy, wh), 78, 23, "Next >", 1);
        draw_button(surface, next_button_x(wx, ww) - 88,
                    next_button_y(wy, wh), 78, 23, "< Back", 1);
    } else if (g_step == STEP_CONFIRM) {
        font_draw_text(surface, wx + 14, wy + 28,
                       "Confirm installation and erase this partition", 12);

        char target_line[96];
        target_description(target_line, sizeof(target_line), g_target, 0);
        font_draw_text(surface, wx + 14, wy + 54, target_line, 0);
        font_draw_text(surface, wx + 14, wy + 78,
                       "The installer will now:", 8);
        font_draw_text(surface, wx + 24, wy + 100,
                       "1. Write a new FAT16/FAT32 VBR/BPB and empty FAT tables.", 0);
        font_draw_text(surface, wx + 24, wy + 120,
                       "2. Recursively copy the complete system payload.", 0);
        font_draw_text(surface, wx + 24, wy + 140,
                       "3. Install Limine UEFI and BIOS offline boot files.", 0);
        font_draw_text(surface, wx + 14, wy + 168,
                       "ALL EXISTING DATA ON THIS PARTITION WILL BE ERASED.", 12);
        font_draw_text(surface, wx + 14, wy + 186,
                       "The GPT/MBR partition table itself is not modified.", 8);

        draw_button(surface, next_button_x(wx, ww),
                    next_button_y(wy, wh), 88, 23, "Install Now", 1);
        draw_button(surface, next_button_x(wx, ww) - 88,
                    next_button_y(wy, wh), 78, 23, "< Back", 1);
    } else if (g_step == STEP_PROGRESS) {
        font_draw_text(surface, wx + 14, wy + 30,
                       "Installing Pixel Neko OS...", 0);
        font_draw_text(surface, wx + 14, wy + 56, g_progress_text, 8);
        int bar_x = wx + 14;
        int bar_y = wy + 82;
        int bar_width = ww - 28;
        draw_inset(surface, bar_x, bar_y, bar_width, 24);
        int fill = (bar_width - 6) * g_progress_percent / 100;
        for (int py = bar_y + 3; py < bar_y + 21; py++) {
            for (int px = bar_x + 3; px < bar_x + 3 + fill; px++) {
                PUT_PIXEL(surface, px, py, 1);
            }
        }
        char percent[8];
        format_uint((unsigned int)g_progress_percent, percent);
        text_append(percent, "%", sizeof(percent));
        font_draw_text(surface, bar_x + bar_width / 2 - 12,
                       bar_y + 7, percent,
                       fill > bar_width / 2 ? 15 : 0);
        font_draw_text(surface, wx + 14, wy + 122, g_progress_subtext, 0);
        font_draw_text(surface, wx + 14, wy + 150,
                       "Do not remove power or the installation media.", 8);
        draw_button(surface, next_button_x(wx, ww),
                    next_button_y(wy, wh), 78, 23, "Working", 0);
    } else if (g_step == STEP_COMPLETE) {
        if (g_install_error == 0) {
            font_draw_text(surface, wx + 14, wy + 28,
                           "Installation Succeeded!", 2);
            font_draw_text(surface, wx + 14, wy + 54,
                           "Pixel Neko OS installed successfully.", 0);
            font_draw_text(surface, wx + 14, wy + 76,
                           "Please remove installation media and reboot.", 0);
            font_draw_text(surface, wx + 14, wy + 104,
                           "Limine UEFI and BIOS boot files are deployed.", 8);
            font_draw_text(surface, wx + 14, wy + 122,
                           "The target disk can boot independently.", 8);
            draw_button(surface, wx + 20, next_button_y(wy, wh) - 4,
                        116, 27, "Reboot Now", 1);
            draw_button(surface, next_button_x(wx, ww) - 120,
                        next_button_y(wy, wh) - 4,
                        120, 27, "Stay on Desktop", 1);
        } else {
            font_draw_text(surface, wx + 14, wy + 28,
                           "Installation Failed", 12);
            char code[32];
            format_uint((unsigned int)(-g_install_error), code);
            char line[64];
            text_copy(line, "Error code: -", sizeof(line));
            text_append(line, code, sizeof(line));
            font_draw_text(surface, wx + 14, wy + 56, line, 12);
            const char* reason = "Could not format or populate the target.";
            if (g_install_error == -10) {
                reason = "The installation payload is missing from the source media.";
            } else if (g_install_error == -11) {
                reason = "Out of memory while copying a payload file.";
            } else if (g_install_error == -12) {
                reason = "An unallocated area large enough for Limine BIOS was not found.";
            } else if (g_install_error == -13) {
                reason = "The destination filesystem could not be mounted.";
            } else if (g_install_error == -14) {
                reason = "A payload file could not be verified after copying.";
            } else if (g_install_error == -15) {
                reason = "The Limine BIOS stage could not be read back safely.";
            }
            font_draw_text(surface, wx + 14, wy + 80, reason, 0);
            font_draw_text(surface, wx + 14, wy + 108,
                           "The existing partition table was not changed.", 8);
            draw_button(surface, next_button_x(wx, ww),
                        next_button_y(wy, wh), 78, 23, "< Back", 1);
        }
    }
}

static int path_join(char* destination, int capacity,
                     const char* directory, const char* name) {
    text_copy(destination, directory, capacity);
    if (destination[0] && destination[strlen(destination) - 1] != '\\' &&
        destination[strlen(destination) - 1] != '/') {
        text_append(destination, "\\", capacity);
    }
    text_append(destination, name, capacity);
    return (int)strlen(destination) < capacity - 1 ? 0 : -1;
}

static void collect_payload_entry(const vfs_dirent_t* entry) {
    if (!g_payload_collector || !entry) return;
    if (g_payload_collector->count >= PAYLOAD_LIST_CAPACITY) {
        g_payload_collector->overflow = 1;
        return;
    }
    g_payload_collector->entries[g_payload_collector->count++] = *entry;
}

static int ensure_directory(const char* path) {
    int result = fat_create_dir(path);
    if (result == 0) return 0;
    vfs_dirent_t entry;
    if (vfs_find_file(path, &entry) == VFS_OK && entry.is_dir) return 0;
    return -1;
}

static int copy_payload_file(const char* source, const char* destination,
                             int file_index, int file_total) {
    vfs_dirent_t source_entry;
    if (vfs_find_file(source, &source_entry) != VFS_OK ||
        source_entry.is_dir || source_entry.size == 0 ||
        source_entry.size > 0xFFFFFFFFull) {
        return -1;
    }
    uint64_t page_count = (source_entry.size + 4095ull) / 4096ull;
    if (page_count > 0xFFFFFFFFull) return -11;
    unsigned char* buffer =
        (unsigned char*)pmm_alloc_pages((unsigned int)page_count);
    if (!buffer) return -11;

    log_printf("[P6 ] Payload copy %d/%d %s -> %s (%u bytes)\n",
               file_index + 1, file_total, source, destination,
               (unsigned int)source_entry.size);
    int read = vfs_read_file(source, buffer, (unsigned int)source_entry.size);
    if (read != (int)source_entry.size) {
        pmm_free_pages(buffer, (unsigned int)page_count);
        return -10;
    }
    int percent = 28;
    if (file_total > 0) percent += file_index * 30 / file_total;
    update_progress_ui(percent, "Releasing complete system payload...",
                       source);
    fat_set_write_progress_cb(on_payload_write_progress);
    int write = vfs_write_file(destination, buffer,
                               (unsigned int)source_entry.size);
    fat_set_write_progress_cb(NULL);
    pmm_free_pages(buffer, (unsigned int)page_count);
    if (write != VFS_OK) return -10;
    log_printf("[P6 ] Payload write returned for %s\n", destination);

    vfs_dirent_t copied;
    int find_result = vfs_find_file(destination, &copied);
    log_printf("[P6 ] Payload verify lookup %s result=%d\n",
               destination, find_result);
    if (find_result != VFS_OK ||
        copied.is_dir || copied.size != source_entry.size) {
        return -14;
    }
    return 0;
}

static int count_payload_files(const char* source_directory) {
    payload_list_t list;
    memset(&list, 0, sizeof(list));
    payload_list_t* previous = g_payload_collector;
    g_payload_collector = &list;
    int result = vfs_list_dir(source_directory, collect_payload_entry);
    g_payload_collector = previous;
    if (result != VFS_OK || list.overflow) return -1;
    int total = 0;
    for (int index = 0; index < list.count; index++) {
        if (list.entries[index].is_dir) {
            char child[256];
            if (path_join(child, sizeof(child), source_directory,
                          list.entries[index].name) != 0) {
                return -1;
            }
            int nested = count_payload_files(child);
            if (nested < 0) return -1;
            total += nested;
        } else {
            total++;
        }
    }
    return total;
}

static int copy_payload_tree(const char* source_directory,
                             const char* destination_directory,
                             int* copied_files, int total_files) {
    payload_list_t list;
    memset(&list, 0, sizeof(list));
    payload_list_t* previous = g_payload_collector;
    g_payload_collector = &list;
    int result = vfs_list_dir(source_directory, collect_payload_entry);
    g_payload_collector = previous;
    if (result != VFS_OK || list.overflow) return -14;

    for (int index = 0; index < list.count; index++) {
        char source[256];
        char destination[256];
        if (path_join(source, sizeof(source), source_directory,
                      list.entries[index].name) != 0 ||
            path_join(destination, sizeof(destination), destination_directory,
                      list.entries[index].name) != 0) {
            return -14;
        }
        if (list.entries[index].is_dir) {
            if (ensure_directory(destination) != 0) return -14;
            int nested = copy_payload_tree(source, destination, copied_files,
                                           total_files);
            if (nested != 0) return nested;
        } else {
            int copy = copy_payload_file(source, destination, *copied_files,
                                         total_files);
            if (copy != 0) return copy;
            (*copied_files)++;
        }
    }
    return 0;
}

static vfs_drive_t* mounted_target_drive(const target_part_t* target) {
    if (!target || !target->part) return NULL;
    for (int index = 0; index < 26; index++) {
        vfs_drive_t* drive = &g_vfs_drives[index];
        if (drive->present && drive->dev == target->part->dev &&
            drive->start_lba == target->part->start_lba) {
            return drive;
        }
    }
    return NULL;
}

static char mount_formatted_target(target_part_t* target,
                                   part_fs_type_t type) {
    target->part->fs_type = type;
    target->part->is_supported = 1;
    vfs_drive_t* existing = mounted_target_drive(target);
    if (existing) {
        existing->fs_type = type;
        if (vfs_remount(existing->letter) == VFS_OK) return existing->letter;
        return 0;
    }

    for (char letter = 'Q'; letter <= 'Z'; letter++) {
        if (!vfs_get_drive(letter) &&
            vfs_mount_partition(target->part, letter) == VFS_OK) {
            return letter;
        }
    }
    for (char letter = 'A'; letter < 'Q'; letter++) {
        if (!vfs_get_drive(letter) &&
            vfs_mount_partition(target->part, letter) == VFS_OK) {
            return letter;
        }
    }
    return 0;
}

static int verify_installed_payload(char letter) {
    static const char* required[] = {
        "boot\\kernel.elf",
        "boot\\limine.conf",
        "boot\\limine\\limine-bios.sys",
        "EFI\\BOOT\\BOOTX64.EFI",
        "system\\PAYLOAD.TXT"
    };
    for (int index = 0;
         index < (int)(sizeof(required) / sizeof(required[0])); index++) {
        char path[96];
        path[0] = letter;
        path[1] = ':';
        path[2] = '\\';
        path[3] = '\0';
        text_append(path, required[index], sizeof(path));
        vfs_dirent_t entry;
        if (vfs_find_file(path, &entry) != VFS_OK || entry.is_dir ||
            entry.size == 0) {
            log_printf("[P6 ] Missing installed payload: %s\n", path);
            return -14;
        }
    }
    return 0;
}

static uint64_t earliest_partition_lba(blkdev_t* dev) {
    uint64_t earliest = dev->total_sectors;
    for (int index = 0; index < g_partition_count; index++) {
        partition_entry_t* part = &g_partitions[index];
        if (part->dev == dev && part->start_lba > 1 &&
            part->start_lba < earliest) {
            earliest = part->start_lba;
        }
    }
    return earliest;
}

static int zero_gpt_entry_range(blkdev_t* dev, uint64_t entries_lba,
                                uint32_t first, uint32_t last,
                                uint32_t entry_size) {
    if (first >= last) return 0;
    uint64_t byte_start = (uint64_t)first * entry_size;
    uint64_t byte_end = (uint64_t)last * entry_size;
    unsigned char buffer[512];
    uint64_t current = byte_start;
    while (current < byte_end) {
        uint64_t sector = entries_lba + current / 512ull;
        uint32_t offset = (uint32_t)(current % 512ull);
        uint64_t remaining = byte_end - current;
        uint32_t available = 512u - offset;
        if (remaining > available) remaining = available;
        if (block_read(dev, sector, 1, buffer) != 0) return -12;
        memset(buffer + offset, 0, (size_t)remaining);
        if (block_write(dev, sector, 1, buffer) != 0) return -12;
        current += remaining;
    }
    return 0;
}

static int crc_gpt_entries(blkdev_t* dev, uint64_t entries_lba,
                           uint64_t byte_count, uint32_t* result) {
    unsigned int pages =
        (unsigned int)((byte_count + 4095ull) / 4096ull);
    unsigned char* buffer = (unsigned char*)pmm_alloc_pages(pages);
    if (!buffer) return -11;
    uint64_t copied = 0;
    int status = 0;
    while (copied < byte_count) {
        uint64_t remaining = byte_count - copied;
        uint32_t bytes = remaining > 512 ? 512 : (uint32_t)remaining;
        uint64_t sector = entries_lba + copied / 512ull;
        if (block_read(dev, sector, 1, buffer + copied) != 0) {
            status = -12;
            break;
        }
        copied += bytes;
    }
    if (status == 0) {
        *result = crc32_update(0xFFFFFFFFu, buffer, (size_t)byte_count) ^
                  0xFFFFFFFFu;
    }
    pmm_free_pages(buffer, pages);
    return status;
}

static int prepare_gpt_bios_embedding(blkdev_t* dev, uint16_t size_a,
                                      uint16_t size_b, uint64_t* location_a,
                                      uint64_t* location_b) {
    unsigned char primary[512];
    if (block_read(dev, 1, 1, primary) != 0 ||
        memcmp(primary, "EFI PART", 8) != 0) {
        return 1;
    }
    uint32_t header_size = read_le32(primary + 12);
    uint64_t backup_lba = read_le64(primary + 32);
    uint64_t entries_lba = read_le64(primary + 72);
    uint32_t entry_count = read_le32(primary + 80);
    uint32_t entry_size = read_le32(primary + 84);
    if (header_size < 92 || header_size > 512 || !backup_lba ||
        backup_lba >= dev->total_sectors || !entries_lba ||
        entry_count == 0 || entry_count > 256 ||
        entry_size < 128 || entry_size > 512 ||
        (512u % entry_size) != 0) {
        return -12;
    }

    unsigned char secondary[512];
    if (block_read(dev, backup_lba, 1, secondary) != 0 ||
        memcmp(secondary, "EFI PART", 8) != 0) {
        return -12;
    }
    uint64_t secondary_entries_lba = read_le64(secondary + 72);
    if (!secondary_entries_lba) return -12;

    uint32_t per_sector = 512u / entry_size;
    uint32_t entry_sectors = (entry_count + per_sector - 1) / per_sector;
    uint32_t max_used = 0;
    for (uint32_t sector_index = 0; sector_index < entry_sectors;
         sector_index++) {
        unsigned char entries[512];
        if (block_read(dev, entries_lba + sector_index, 1, entries) != 0) {
            return -12;
        }
        for (uint32_t item = 0; item < per_sector; item++) {
            uint32_t index = sector_index * per_sector + item;
            if (index >= entry_count) break;
            unsigned char* entry = entries + item * entry_size;
            int used = 0;
            for (uint32_t byte = 0; byte < 16; byte++) {
                if (entry[byte]) {
                    used = 1;
                    break;
                }
            }
            if (used) max_used = index;
        }
    }

    uint64_t raw_a = (entries_lba + 32ull) * 512ull - size_a;
    uint64_t raw_b =
        (secondary_entries_lba + 32ull) * 512ull - size_b;
    *location_a = raw_a & ~511ull;
    *location_b = raw_b & ~511ull;
    if (*location_a < entries_lba * 512ull ||
        *location_a % 512ull ||
        *location_b < secondary_entries_lba * 512ull ||
        *location_b % 512ull) {
        return -12;
    }

    uint64_t new_entry_sectors = *location_a / 512ull - entries_lba;
    uint32_t new_count = (uint32_t)(new_entry_sectors * per_sector);
    if (new_count <= max_used || new_count > entry_count) return -12;

    if (zero_gpt_entry_range(dev, entries_lba, max_used + 1, new_count,
                             entry_size) != 0 ||
        zero_gpt_entry_range(dev, secondary_entries_lba, max_used + 1,
                             new_count, entry_size) != 0) {
        return -12;
    }

    uint64_t entry_bytes = (uint64_t)new_count * entry_size;
    uint32_t entries_crc = 0;
    if (crc_gpt_entries(dev, entries_lba, entry_bytes, &entries_crc) != 0 ||
        crc_gpt_entries(dev, secondary_entries_lba, entry_bytes,
                        &entries_crc) != 0) {
        return -12;
    }

    put_le32(primary + 80, new_count);
    put_le32(primary + 88, entries_crc);
    put_le32(primary + 16, 0);
    put_le32(primary + 16,
             crc32_update(0xFFFFFFFFu, primary, header_size) ^ 0xFFFFFFFFu);
    if (block_write(dev, 1, 1, primary) != 0) return -12;

    put_le32(secondary + 80, new_count);
    put_le32(secondary + 88, entries_crc);
    put_le32(secondary + 16, 0);
    put_le32(secondary + 16,
             crc32_update(0xFFFFFFFFu, secondary, header_size) ^
                 0xFFFFFFFFu);
    if (block_write(dev, backup_lba, 1, secondary) != 0) return -12;

    log_printf("[P6 ] GPT embedded Limine stage2 at LBA=%llu/%llu, entries=%u\n",
               (unsigned long long)(*location_a / 512ull),
               (unsigned long long)(*location_b / 512ull), new_count);
    return 0;
}

static int select_stage2_locations(blkdev_t* dev, uint16_t size_a,
                                   uint16_t size_b, uint64_t* location_a,
                                   uint64_t* location_b) {
    int gpt_result =
        prepare_gpt_bios_embedding(dev, size_a, size_b, location_a,
                                   location_b);
    if (gpt_result <= 0) return gpt_result;

    size_t stage2_sectors = (size_a + size_b) / 512u;
    *location_a = 1;
    *location_b = 1 + size_a / 512u;
    uint64_t first_partition = earliest_partition_lba(dev);
    if (1 + stage2_sectors > first_partition) {
        log_printf("[P6 ] No safe Limine BIOS gap before LBA %llu\n",
                   (unsigned long long)first_partition);
        return -12;
    }
    return 0;
}

static int install_limine_bios(blkdev_t* dev) {
    const unsigned char* image = binary_limine_hdd_bin_data;
    size_t image_size = sizeof(binary_limine_hdd_bin_data);
    if (image_size <= 512) return -15;
    size_t stage2_size = image_size - 512;
    size_t stage2_sectors = (stage2_size + 511) / 512;
    uint16_t size_a =
        (uint16_t)((stage2_sectors / 2) * 512 +
                   (stage2_sectors % 2 ? 512 : 0));
    uint16_t size_b = (uint16_t)((stage2_sectors / 2) * 512);

    uint64_t location_a;
    uint64_t location_b;
    if (select_stage2_locations(dev, size_a, size_b, &location_a,
                                &location_b) != 0) {
        return -12;
    }

    unsigned char original_mbr[512];
    if (block_read(dev, 0, 1, original_mbr) != 0) return -15;
    unsigned char bootsector[512];
    memcpy(bootsector, image, sizeof(bootsector));
    memcpy(bootsector + 440, original_mbr + 440, 70);
    put_be16(bootsector + 0x1A4, size_a);
    put_be16(bootsector + 0x1A6, size_b);
    put_be64(bootsector + 0x1A8, location_a * 512ull);
    put_be64(bootsector + 0x1B0, location_b * 512ull);

    log_printf("[P6 ] Writing Limine stage2 first half at LBA %llu\n",
               (unsigned long long)location_a);
    if (block_write(dev, location_a, (uint32_t)(size_a / 512),
                    image + 512) != 0) {
        return -15;
    }
    log_printf("[P6 ] Writing Limine stage2 second half at LBA %llu\n",
               (unsigned long long)location_b);
    size_t tail_offset = 512 + size_a;
    size_t tail_size = image_size - tail_offset;
    size_t tail_sectors = (tail_size + 511) / 512;
    unsigned int tail_pages =
        (unsigned int)((tail_sectors * 512 + 4095) / 4096);
    unsigned char* tail = (unsigned char*)pmm_alloc_pages(tail_pages);
    if (!tail) return -11;
    memset(tail, 0, (size_t)tail_pages * 4096);
    memcpy(tail, image + tail_offset, tail_size);
    int tail_result =
        block_write(dev, location_b, (uint32_t)tail_sectors, tail);
    pmm_free_pages(tail, tail_pages);
    if (tail_result != 0) return -15;
    log_printf("[P6 ] Writing Limine stage1 to LBA 0\n");
    if (block_write(dev, 0, 1, bootsector) != 0) return -15;

    unsigned char verify[512];
    if (block_read(dev, 0, 1, verify) != 0 ||
        verify[510] != 0x55 || verify[511] != 0xAA ||
        memcmp(verify + 446, original_mbr + 446, 64) != 0) {
        return -15;
    }
    log_printf("[P6 ] Limine BIOS deployed: stage2 LBA=%llu/%llu stage1 LBA=0\n",
               (unsigned long long)location_a,
               (unsigned long long)location_b);
    return 0;
}

static int do_installation(target_part_t* target, part_fs_type_t type) {
    if (!target || !target->part || !target->part->dev) return -1;
    block_device_t* dev = target->part->dev;
    if (!target_supports_format(target, type)) return -2;

    update_progress_ui(5, "Formatting the selected partition...",
                       type == PART_FS_FAT32
                           ? "Creating FAT32 VBR, BPB, FSInfo and mirrored FAT"
                           : "Creating FAT16 VBR, BPB and mirrored FAT");
    log_printf("[P6 ] Format target disk=%s lba=%llu sectors=%llu as %s\n",
               dev->name,
               (unsigned long long)target->part->start_lba,
               (unsigned long long)target->part->total_sectors,
               type == PART_FS_FAT32 ? "FAT32" : "FAT16");
    if (format_partition(dev, target->part->start_lba,
                         target->part->total_sectors, type) != 0) {
        return -2;
    }

    update_progress_ui(16, "Mounting the new filesystem...",
                       "Refreshing VFS volume metadata");
    char target_letter = mount_formatted_target(target, type);
    if (!target_letter) return -13;
    log_printf("[P6 ] Formatted target mounted as %c:\n", target_letter);

    char source_letter = vfs_get_boot_cdrom_letter();
    if (!source_letter) {
        log_printf("[P6 ] Installation source payload media not found.\n");
        return -10;
    }
    char source_root[32];
    source_root[0] = source_letter;
    source_root[1] = ':';
    source_root[2] = '\\';
    source_root[3] = 'p';
    source_root[4] = 'a';
    source_root[5] = 'y';
    source_root[6] = 'l';
    source_root[7] = 'o';
    source_root[8] = 'a';
    source_root[9] = 'd';
    source_root[10] = '\0';
    vfs_dirent_t payload_entry;
    if (vfs_find_file(source_root, &payload_entry) != VFS_OK ||
        !payload_entry.is_dir) {
        log_printf("[P6 ] Source payload directory %s is missing.\n",
                   source_root);
        return -10;
    }
    int total_files = count_payload_files(source_root);
    if (total_files <= 0) return -10;
    log_printf("[P6 ] Source payload %s contains %d file(s).\n",
               source_root, total_files);

    char destination_root[4];
    destination_root[0] = target_letter;
    destination_root[1] = ':';
    destination_root[2] = '\\';
    destination_root[3] = '\0';
    int copied_files = 0;
    update_progress_ui(24, "Releasing complete system payload...",
                       "Recursively copying boot files, documents and manifest");
    int copy_result = copy_payload_tree(source_root, destination_root,
                                        &copied_files, total_files);
    if (copy_result != 0) return copy_result;
    if (copied_files != total_files) return -14;

    update_progress_ui(66, "Verifying the installed payload...",
                       "Checking kernel, configuration, EFI and BIOS files");
    fat_flush_all();
    int verify_result = verify_installed_payload(target_letter);
    if (verify_result != 0) return verify_result;

    update_progress_ui(76, "Deploying Limine BIOS offline boot...",
                       "Writing Stage 1 and embedding Stage 2 without changing partitions");
    int bios_result = install_limine_bios(dev);
    if (bios_result != 0) return bios_result;

    update_progress_ui(92, "Finalizing independent boot...",
                       "UEFI EFI/BOOT and BIOS Limine payloads are ready");
    fat_flush_all();
    vfs_get_drive_space(target_letter, NULL, NULL, NULL);
    update_progress_ui(100, "Installation completed successfully!",
                       "Remove the installation media, then reboot");
    log_printf("[P6 ] Installation complete on %c: copied=%d UEFI=ready BIOS=ready\n",
               target_letter, copied_files);
    return 0;
}

static int point_in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void begin_install(void) {
    g_step = STEP_PROGRESS;
    update_progress_ui(0, "Starting installation...",
                       "Preparing the selected existing partition");
    g_install_error = do_installation(g_target, g_format_type);
    g_step = STEP_COMPLETE;
    redraw = 1;
}

void installer_click(unsigned char* surface, int mx, int my,
                     int wx, int wy, int ww, int wh) {
    (void)surface;
    if (g_step == STEP_SELECT) {
        int list_x = wx + 14;
        int list_y = wy + 66;
        int list_width = ww - 28;
        int visible = g_target_count > 8 ? 8 : g_target_count;
        if (point_in_rect(mx, my, list_x, list_y,
                          list_width, visible * 22 + 12)) {
            int clicked = (my - (list_y + 6)) / 22;
            if (clicked >= 0 && clicked < visible) {
                g_selected_idx = clicked;
                redraw = 1;
            }
            return;
        }
        if (point_in_rect(mx, my, next_button_x(wx, ww),
                          next_button_y(wy, wh), 78, 23) &&
            g_target_count > 0) {
            g_target = &g_targets[g_selected_idx];
            g_format_type =
                target_supports_format(g_target, PART_FS_FAT32)
                    ? PART_FS_FAT32 : PART_FS_FAT16;
            g_step = STEP_FORMAT;
            log_printf("[P6 ] Installer target selected: disk=%s lba=%llu size=%u MB fs=%s\n",
                       g_target->disk_name,
                       (unsigned long long)g_target->part->start_lba,
                       g_target->total_mb, g_target->fs_name);
            redraw = 1;
            return;
        }
        if (point_in_rect(mx, my, wx + 14, next_button_y(wy, wh),
                          78, 23)) {
            insv = 0;
            redraw = 1;
        }
    } else if (g_step == STEP_FORMAT) {
        int box_x = wx + 14;
        int box_width = ww - 28;
        if (point_in_rect(mx, my, box_x, wy + 76, box_width, 42) &&
            target_supports_format(g_target, PART_FS_FAT32)) {
            g_format_type = PART_FS_FAT32;
            redraw = 1;
            return;
        }
        if (point_in_rect(mx, my, box_x, wy + 130, box_width, 42) &&
            target_supports_format(g_target, PART_FS_FAT16)) {
            g_format_type = PART_FS_FAT16;
            redraw = 1;
            return;
        }
        if (point_in_rect(mx, my, next_button_x(wx, ww),
                          next_button_y(wy, wh), 78, 23)) {
            g_step = STEP_CONFIRM;
            redraw = 1;
            return;
        }
        if (point_in_rect(mx, my, next_button_x(wx, ww) - 88,
                          next_button_y(wy, wh), 78, 23)) {
            g_step = STEP_SELECT;
            redraw = 1;
        }
    } else if (g_step == STEP_CONFIRM) {
        if (point_in_rect(mx, my, next_button_x(wx, ww),
                          next_button_y(wy, wh), 88, 23)) {
            begin_install();
            return;
        }
        if (point_in_rect(mx, my, next_button_x(wx, ww) - 88,
                          next_button_y(wy, wh), 78, 23)) {
            g_step = STEP_FORMAT;
            redraw = 1;
        }
    } else if (g_step == STEP_COMPLETE) {
        if (g_install_error == 0) {
            if (point_in_rect(mx, my, wx + 20,
                              next_button_y(wy, wh) - 4, 116, 27)) {
                sys_reboot();
                return;
            }
            if (point_in_rect(mx, my, next_button_x(wx, ww) - 120,
                              next_button_y(wy, wh) - 4, 120, 27)) {
                insv = 0;
                redraw = 1;
            }
        } else if (point_in_rect(mx, my, next_button_x(wx, ww),
                                 next_button_y(wy, wh), 78, 23)) {
            g_step = STEP_SELECT;
            scan_targets();
            redraw = 1;
        }
    }
}
