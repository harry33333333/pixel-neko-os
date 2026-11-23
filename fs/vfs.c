#include "vfs.h"
#include "ahci.h"
#include "apic.h"
#include "ata.h"
#include "exfat.h"
#include "fat.h"
#include "iso9660.h"
#include "limine.h"
#include "log.h"
#include "settings.h"
#include "string.h"
#include "usb_storage.h"

extern volatile struct limine_kernel_file_request kernel_file_request;

vfs_drive_t g_vfs_drives[26];

static iso_fs_t g_iso_filesystems[4];
static exfat_fs_t g_exfat_filesystems[26];
static uint64_t g_last_media_poll_tick;
static uint32_t g_last_usb_storage_gen;

typedef enum {
    VFS_FS_NONE = 0,
    VFS_FS_FAT,
    VFS_FS_EXFAT,
    VFS_FS_ISO
} vfs_filesystem_kind_t;

static vfs_filesystem_kind_t g_filesystem_kind[26];

static void copy_text(char* target, unsigned int capacity, const char* source) {
    unsigned int index = 0;
    if (!target || !capacity) return;
    while (source && source[index] && index + 1 < capacity) {
        target[index] = source[index];
        index++;
    }
    target[index] = '\0';
}

static char normalize_letter(char letter) {
    return letter >= 'a' && letter <= 'z' ? (char)(letter - 32) : letter;
}

static char lower_ascii(char value) {
    return value >= 'A' && value <= 'Z' ? (char)(value + 32) : value;
}

static const char* path_relative(const char* path) {
    if (!path) return NULL;
    if (path[0] && path[1] == ':') path += 2;
    while (*path == '\\' || *path == '/') path++;
    return path;
}

static bool path_has_prefix(const char* path, const char* prefix) {
    const char* relative = path_relative(path);
    if (!relative || !prefix) return false;
    while (*prefix) {
        if (lower_ascii(*relative) != lower_ascii(*prefix)) return false;
        relative++;
        prefix++;
    }
    return true;
}

static bool path_name_equals(const char* path, const char* name) {
    const char* relative = path_relative(path);
    if (!relative) return false;
    const char* base = relative;
    for (const char* cursor = relative; *cursor; cursor++) {
        if (*cursor == '\\' || *cursor == '/') base = cursor + 1;
    }
    while (*base && *name) {
        if (lower_ascii(*base) != lower_ascii(*name)) return false;
        base++;
        name++;
    }
    return *base == '\0' && *name == '\0';
}

bool vfs_path_is_protected(const char* path) {
    const char* relative = path_relative(path);
    if (!relative || !*relative) return false;
    if (log_disk_is_protected_path(path, true)) return true;
    if (path_has_prefix(path, "apps\\") ||
        path_has_prefix(path, "boot\\") ||
        path_has_prefix(path, "system\\") ||
        path_has_prefix(path, "pdevice\\") ||
        path_has_prefix(path, "efi\\")) {
        return true;
    }
    if (path_name_equals(path, "apps") ||
        path_name_equals(path, "boot") ||
        path_name_equals(path, "system") ||
        path_name_equals(path, "pdevice") ||
        path_name_equals(path, "efi") ||
        path_name_equals(path, "desktop") ||
        path_name_equals(path, "limine.conf") ||
        path_name_equals(path, ".desktop-layout") ||
        path_name_equals(path, "File Management.pnk") ||
        path_name_equals(path, "About This Machine.pnk")) {
        return true;
    }
    return false;
}

static bool vfs_live_desktop_locked(const char* path) {
    return settings_get_boot_mode() == SETTINGS_BOOT_LIVE &&
           (path_has_prefix(path, "desktop\\") ||
            path_name_equals(path, "desktop"));
}

const char* vfs_get_fs_name(part_fs_type_t type) {
    switch (type) {
        case PART_FS_FAT12: return "FAT12";
        case PART_FS_FAT16: return "FAT16";
        case PART_FS_FAT32: return "FAT32";
        case PART_FS_NTFS: return "NTFS";
        case PART_FS_EXFAT: return "exFAT";
        case PART_FS_EXT: return "EXT";
        default: return "Unknown";
    }
}

vfs_drive_t* vfs_get_drive(char letter) {
    letter = normalize_letter(letter);
    if (letter < 'A' || letter > 'Z') return NULL;
    vfs_drive_t* drive = &g_vfs_drives[letter - 'A'];
    return drive->present ? drive : NULL;
}

static vfs_drive_t* initialize_drive(char letter) {
    if (letter < 'A' || letter > 'Z') return NULL;
    vfs_drive_t* drive = &g_vfs_drives[letter - 'A'];
    memset(drive, 0, sizeof(*drive));
    drive->letter = letter;
    drive->present = 1;
    g_filesystem_kind[letter - 'A'] = VFS_FS_NONE;
    return drive;
}

static bool mount_partition(partition_entry_t* part, char letter) {
    if (!part || !part->dev) return false;
    vfs_drive_t* drive = initialize_drive(letter);
    if (!drive) return false;
    drive->drv_type = part->dev->type == BLK_TYPE_USB ? VFS_DRV_USB :
                      VFS_DRV_HDD;
    drive->fs_type = part->fs_type;
    drive->is_supported = part->is_supported ? 1 : 0;
    drive->dev = part->dev;
    drive->start_lba = part->start_lba;
    drive->total_sectors = part->total_sectors;
    drive->total_kb = part->total_sectors *
                      (part->dev->sector_size / 1024u);
    copy_text(drive->label, sizeof(drive->label), part->label);
    if (part->is_supported && part->fs_type == PART_FS_EXFAT) {
        exfat_fs_t* exfat = &g_exfat_filesystems[letter - 'A'];
        if (exfat_mount(part->dev, part->start_lba, exfat) == 0) {
            drive->mounted = 1;
            g_filesystem_kind[letter - 'A'] = VFS_FS_EXFAT;
            if (exfat->volume_label[0]) {
                copy_text(drive->label, sizeof(drive->label),
                          exfat->volume_label);
            }
        }
    } else if (part->is_supported &&
               (part->fs_type == PART_FS_FAT12 ||
                part->fs_type == PART_FS_FAT16 ||
                part->fs_type == PART_FS_FAT32)) {
        if (fat_mount_dev(letter, part->dev, part->start_lba) == 0) {
            drive->mounted = 1;
            g_filesystem_kind[letter - 'A'] = VFS_FS_FAT;
        }
    }
    if (!drive->label[0]) {
        const char* fallback = drive->drv_type == VFS_DRV_USB
                                   ? "Removable Disk" : "Local Disk";
        copy_text(drive->label, sizeof(drive->label), fallback);
    }
    return drive->present != 0;
}

int vfs_mount_partition(partition_entry_t* part, char letter) {
    letter = normalize_letter(letter);
    if (!part || letter < 'A' || letter > 'Z' ||
        g_vfs_drives[letter - 'A'].present) {
        return VFS_ERR_NO_DRIVE;
    }
    if (!part->dev || !part->is_supported) return VFS_ERR_UNSUPPORTED;
    return mount_partition(part, letter) ? VFS_OK : VFS_ERR_UNSUPPORTED;
}

static void mount_optical(blkdev_t* dev, char letter) {
    vfs_drive_t* drive = initialize_drive(letter);
    if (!drive) return;
    drive->drv_type = VFS_DRV_CDROM;
    drive->fs_type = PART_FS_UNKNOWN;
    drive->is_supported = 1;
    drive->is_readonly = 1;
    drive->dev = dev;
    drive->media_generation = dev->media_generation;
    drive->total_sectors = dev->total_sectors;
    drive->total_kb = dev->total_sectors *
                      (dev->sector_size / 1024u);
    if (dev->media_state == MEDIA_PRESENT && dev->total_sectors) {
        for (int index = 0; index < 4; index++) {
            if (g_iso_filesystems[index].mounted) continue;
            if (iso_mount(dev, &g_iso_filesystems[index]) != 0) continue;
            drive->mounted = 1;
            drive->total_sectors = g_iso_filesystems[index].total_sectors;
            drive->total_kb = drive->total_sectors * 2;
            copy_text(drive->label, sizeof(drive->label),
                      g_iso_filesystems[index].volume_label[0]
                          ? g_iso_filesystems[index].volume_label
                          : "Pixel Neko Installation Media");
            g_filesystem_kind[letter - 'A'] = VFS_FS_ISO;
            break;
        }
    }
    if (!drive->mounted) {
        copy_text(drive->label, sizeof(drive->label),
                  dev->media_state == MEDIA_PRESENT ?
                      "Optical Drive (No ISO)" : "Optical Drive (Empty)");
    }
}

static bool mount_raw_iso(blkdev_t* dev, char letter) {
    if (!dev || letter < 'A' || letter > 'Z' ||
        g_vfs_drives[letter - 'A'].present) {
        return false;
    }
    for (int index = 0; index < 4; index++) {
        if (g_iso_filesystems[index].mounted &&
            g_iso_filesystems[index].dev != dev) {
            continue;
        }
        if (iso_mount(dev, &g_iso_filesystems[index]) != 0) continue;
        vfs_drive_t* drive = initialize_drive(letter);
        if (!drive) return false;
        drive->drv_type = VFS_DRV_CDROM;
        drive->fs_type = PART_FS_UNKNOWN;
        drive->is_supported = 1;
        drive->is_readonly = 1;
        drive->mounted = 1;
        drive->dev = dev;
        drive->total_sectors = g_iso_filesystems[index].total_sectors;
        drive->total_kb = drive->total_sectors * 2;
        drive->media_generation = dev->media_generation;
        copy_text(drive->label, sizeof(drive->label),
                  g_iso_filesystems[index].volume_label[0]
                      ? g_iso_filesystems[index].volume_label
                      : "Pixel Neko Live Media");
        g_filesystem_kind[letter - 'A'] = VFS_FS_ISO;
        log_printf("[VFS ] %c: ISO9660 live volume on %s (%llu sectors)\n",
                   letter, dev->name,
                   (unsigned long long)drive->total_sectors);
        return true;
    }
    return false;
}

static void clear_optical_mount(vfs_drive_t* drive) {
    if (!drive || !drive->dev) return;
    for (int index = 0; index < 4; index++) {
        if (g_iso_filesystems[index].dev == drive->dev) {
            memset(&g_iso_filesystems[index], 0,
                   sizeof(g_iso_filesystems[index]));
        }
    }
    drive->mounted = 0;
    drive->fs_type = PART_FS_UNKNOWN;
    drive->is_supported = 1;
    drive->is_readonly = 1;
    drive->total_sectors = drive->dev->total_sectors;
    drive->total_kb = drive->total_sectors *
                      (drive->dev->sector_size / 1024u);
    g_filesystem_kind[drive->letter - 'A'] = VFS_FS_NONE;
}

static void clear_floppy_mount(vfs_drive_t* drive) {
    if (!drive) return;
    fat_unmount_dev(drive->letter);
    drive->mounted = 0;
    drive->fs_type = PART_FS_UNKNOWN;
    drive->is_supported = 0;
    drive->is_readonly = 0;
    drive->total_sectors = 0;
    drive->total_kb = 0;
    drive->free_kb = 0;
    drive->is_free_known = 0;
    drive->label[0] = '\0';
    g_filesystem_kind[drive->letter - 'A'] = VFS_FS_NONE;
}

static bool mount_floppy(blkdev_t* dev, char letter) {
    if (!dev || dev->type != BLK_TYPE_FLOPPY || !dev->total_sectors) {
        return false;
    }
    vfs_drive_t* drive = initialize_drive(letter);
    if (!drive) return false;

    drive->drv_type = VFS_DRV_FLOPPY;
    drive->fs_type = PART_FS_UNKNOWN;
    drive->is_supported = 0;
    drive->is_readonly = dev->is_readonly;
    drive->dev = dev;
    drive->start_lba = 0;
    drive->total_sectors = dev->total_sectors;
    drive->total_kb =
        dev->total_sectors * (dev->sector_size / 1024u);
    drive->media_generation = dev->media_generation;
    copy_text(drive->label, sizeof(drive->label), "Floppy Disk");

    fat_unmount_dev(letter);
    if (fat_mount_dev(letter, dev, 0) == 0) {
        int index = letter - 'A';
        if (g_volumes[index].type == 12) drive->fs_type = PART_FS_FAT12;
        else if (g_volumes[index].type == 16) drive->fs_type = PART_FS_FAT16;
        else drive->fs_type = PART_FS_FAT32;
        drive->is_supported = 1;
        drive->mounted = 1;
        g_filesystem_kind[index] = VFS_FS_FAT;
    } else {
        copy_text(drive->label, sizeof(drive->label),
                  "Floppy Disk (Unsupported)");
    }
    return true;
}

static bool refresh_floppy_drive(blkdev_t* dev) {
    if (!dev || dev->type != BLK_TYPE_FLOPPY ||
        dev->unit < 0 || dev->unit >= 2) {
        return false;
    }

    media_state_t state = block_check_media(dev);
    char letter = (char)('A' + dev->unit);
    vfs_drive_t* drive = &g_vfs_drives[letter - 'A'];

    if (state != MEDIA_PRESENT || !dev->total_sectors) {
        if (!drive->present) return false;
        clear_floppy_mount(drive);
        drive->present = 0;
        log_printf("[VFS ] %c: floppy media removed\n", letter);
        return true;
    }

    if (!drive->present) {
        if (!mount_floppy(dev, letter)) return false;
        log_printf("[VFS ] %c: floppy inserted (%llu KiB)\n", letter,
                   (unsigned long long)dev->total_sectors / 2);
        return true;
    }

    if (drive->media_generation == dev->media_generation &&
        drive->total_sectors == dev->total_sectors) {
        return false;
    }

    clear_floppy_mount(drive);
    if (!mount_floppy(dev, letter)) return false;
    log_printf("[VFS ] %c: floppy changed (%llu KiB)\n", letter,
               (unsigned long long)dev->total_sectors / 2);
    return true;
}

static bool refresh_optical_drive(vfs_drive_t* drive) {
    if (!drive || drive->drv_type != VFS_DRV_CDROM || !drive->dev) {
        return false;
    }
    blkdev_t* dev = drive->dev;
    media_state_t state = block_check_media(dev);
    if (dev->media_generation == drive->media_generation &&
        state == dev->media_state) {
        return false;
    }

    drive->media_generation = dev->media_generation;
    clear_optical_mount(drive);
    if (state != MEDIA_PRESENT || !dev->total_sectors) {
        copy_text(drive->label, sizeof(drive->label),
                  "Optical Drive (Empty)");
        log_printf("[VFS ] %c: optical medium removed\n", drive->letter);
        return true;
    }

    for (int index = 0; index < 4; index++) {
        if (g_iso_filesystems[index].mounted) continue;
        if (iso_mount(dev, &g_iso_filesystems[index]) != 0) continue;
        drive->mounted = 1;
        drive->total_sectors = g_iso_filesystems[index].total_sectors;
        drive->total_kb = drive->total_sectors * 2;
        copy_text(drive->label, sizeof(drive->label),
                  g_iso_filesystems[index].volume_label[0]
                      ? g_iso_filesystems[index].volume_label
                      : "Optical Media");
        g_filesystem_kind[drive->letter - 'A'] = VFS_FS_ISO;
        log_printf("[VFS ] %c: reloaded optical volume %s\n",
                   drive->letter, drive->label);
        return true;
    }

    copy_text(drive->label, sizeof(drive->label),
              "Optical Drive (No ISO)");
    log_printf("[VFS ] %c: medium inserted but no ISO9660 volume found\n",
               drive->letter);
    return true;
}

void vfs_mount_initial_floppies(void) {
    for (int index = 0; index < blkdev_count(); index++) {
        blkdev_t* dev = blkdev_get(index);
        if (!dev || dev->type != BLK_TYPE_FLOPPY ||
            dev->unit < 0 || dev->unit >= 2) {
            continue;
        }
        char letter = (char)('A' + dev->unit);
        if (g_vfs_drives[letter - 'A'].present) continue;
        if (block_check_media(dev) == MEDIA_PRESENT && dev->total_sectors) {
            (void)mount_floppy(dev, letter);
        }
    }
}

void vfs_init(void) {
    memset(g_vfs_drives, 0, sizeof(g_vfs_drives));
    memset(g_iso_filesystems, 0, sizeof(g_iso_filesystems));
    memset(g_exfat_filesystems, 0, sizeof(g_exfat_filesystems));
    memset(g_filesystem_kind, 0, sizeof(g_filesystem_kind));
    g_last_media_poll_tick = apic_get_ticks();
    fat_init();

    vfs_mount_initial_floppies();

    char local_letter = 'C';
    char removable_letter = 'E';
    const char optical_letter = 'D';
    for (int sup_pass = 0; sup_pass < 2; sup_pass++) {
        for (int pass = 0; pass < 2; pass++) {
            for (int index = 0; index < g_partition_count; index++) {
                partition_entry_t* part = &g_partitions[index];
                if (part->is_efi_system) continue;
                if ((sup_pass == 0) != (part->is_supported != 0)) continue;
                bool removable = part->dev->type == BLK_TYPE_USB;
                if ((pass == 0) != removable) continue;
                char* cursor = removable ? &removable_letter : &local_letter;
                while (*cursor == optical_letter ||
                       (removable && *cursor < 'E') ||
                       (!removable && *cursor == 'E') ||
                       (*cursor >= 'A' && *cursor <= 'Z' &&
                        g_vfs_drives[*cursor - 'A'].present)) {
                    (*cursor)++;
                }
                if (*cursor > 'Z') continue;
                if (mount_partition(part, *cursor)) (*cursor)++;
            }
        }
    }

    char optical_cursor = optical_letter;
    for (int index = 0; index < blkdev_count(); index++) {
        blkdev_t* dev = blkdev_get(index);
        if (!dev || dev->type != BLK_TYPE_ATAPI) continue;
        while (optical_cursor <= 'Z' &&
               g_vfs_drives[optical_cursor - 'A'].present) {
            optical_cursor++;
        }
        if (optical_cursor > 'Z') break;
        mount_optical(dev, optical_cursor++);
    }

    /*
     * A dd'ed hybrid ISO boots from USB as a raw disk, not as ATAPI media.
     * Retry all non-optical block devices through the sector-translating ISO
     * reader so a USB Live session can find APPS\ and PDEVICE\.
     */
    for (int index = 0; index < blkdev_count(); index++) {
        blkdev_t* dev = blkdev_get(index);
        if (!dev || dev->type == BLK_TYPE_ATAPI ||
            dev->type == BLK_TYPE_FLOPPY || !dev->total_sectors) {
            continue;
        }
        bool already_mounted = false;
        for (char letter = 'A'; letter <= 'Z'; letter++) {
            vfs_drive_t* drive = &g_vfs_drives[letter - 'A'];
            if (drive->present && drive->mounted && drive->dev == dev) {
                already_mounted = true;
                break;
            }
        }
        if (already_mounted) continue;
        while (optical_cursor <= 'Z' &&
               g_vfs_drives[optical_cursor - 'A'].present) {
            optical_cursor++;
        }
        if (optical_cursor > 'Z') break;
        if (mount_raw_iso(dev, optical_cursor)) optical_cursor++;
    }

    g_last_usb_storage_gen = usb_storage_get_generation();
    int mounted = 0;
    for (int index = 0; index < 26; index++) {
        if (g_vfs_drives[index].mounted) mounted++;
    }
    log_printf("[VFS ] Mounted %d drive(s); C: local, D: optical, E: removable\n",
               mounted);
}

static bool vfs_blkdev_is_active_usb(const blkdev_t* dev) {
    if (!dev || dev->type != BLK_TYPE_USB) return false;
    int total = blkdev_count();
    for (int i = 0; i < total; i++) {
        if (blkdev_get(i) == dev) return true;
    }
    return false;
}

static bool vfs_dev_is_mounted(const blkdev_t* dev) {
    if (!dev) return false;
    for (int i = 0; i < 26; i++) {
        if (g_vfs_drives[i].present && g_vfs_drives[i].dev == dev) {
            return true;
        }
    }
    return false;
}

static char vfs_alloc_removable_letter(void) {
    for (char letter = 'E'; letter <= 'Z'; letter++) {
        if (!g_vfs_drives[letter - 'A'].present) return letter;
    }
    return 0;
}

bool vfs_sync_usb_drives(void) {
    uint32_t gen = usb_storage_get_generation();
    if (gen == g_last_usb_storage_gen) return false;
    g_last_usb_storage_gen = gen;

    bool changed = false;
    for (int i = 0; i < 26; i++) {
        vfs_drive_t* drive = &g_vfs_drives[i];
        if (!drive->present) continue;
        bool is_usb = (drive->drv_type == VFS_DRV_USB);
        if (!is_usb && drive->dev &&
            drive->drv_type == VFS_DRV_CDROM &&
            !vfs_blkdev_is_active_usb(drive->dev)) {
            /* Keep non-USB optical drives (ATAPI/HDD raw ISO) untouched */
            bool found = false;
            for (int b = 0; b < blkdev_count(); b++) {
                if (blkdev_get(b) == drive->dev) {
                    found = true;
                    break;
                }
            }
            if (found) continue;
            is_usb = true;
        }
        if (!is_usb) continue;
        if (vfs_blkdev_is_active_usb(drive->dev)) continue;

        char letter = drive->letter;
        if (g_filesystem_kind[i] == VFS_FS_FAT) {
            fat_unmount_dev(letter);
        } else if (g_filesystem_kind[i] == VFS_FS_EXFAT) {
            memset(&g_exfat_filesystems[i], 0, sizeof(g_exfat_filesystems[i]));
        } else if (g_filesystem_kind[i] == VFS_FS_ISO) {
            for (int iso_i = 0; iso_i < 4; iso_i++) {
                if (g_iso_filesystems[iso_i].dev == drive->dev) {
                    memset(&g_iso_filesystems[iso_i], 0,
                           sizeof(g_iso_filesystems[iso_i]));
                }
            }
        }
        g_filesystem_kind[i] = VFS_FS_NONE;
        part_remove_device(drive->dev);
        memset(drive, 0, sizeof(*drive));
        log_printf("[VFS ] %c: USB drive detached\n", letter);
        changed = true;
    }

    int total_blk = blkdev_count();
    for (int b = 0; b < total_blk; b++) {
        blkdev_t* dev = blkdev_get(b);
        if (!dev || dev->type != BLK_TYPE_USB || !dev->total_sectors) {
            continue;
        }
        if (vfs_dev_is_mounted(dev)) continue;

        part_scan_device(dev);
        bool any_mounted = false;
        for (int p = 0; p < g_partition_count; p++) {
            partition_entry_t* part = &g_partitions[p];
            if (part->dev != dev || part->is_efi_system) continue;
            char letter = vfs_alloc_removable_letter();
            if (!letter) break;
            if (mount_partition(part, letter)) {
                log_printf("[VFS ] %c: mounted USB volume %s (%llu sectors)\n",
                           letter, dev->name,
                           (unsigned long long)part->total_sectors);
                any_mounted = true;
                changed = true;
            }
        }
        if (!any_mounted) {
            char letter = vfs_alloc_removable_letter();
            if (letter && mount_raw_iso(dev, letter)) {
                changed = true;
            }
        }
    }
    return changed;
}

bool vfs_poll_media(void) {
    bool changed = vfs_sync_usb_drives();
    uint64_t now = apic_get_ticks();
    if (now - g_last_media_poll_tick < 100) return changed;
    g_last_media_poll_tick = now;
    for (int index = 0; index < blkdev_count(); index++) {
        blkdev_t* dev = blkdev_get(index);
        if (dev && dev->type == BLK_TYPE_FLOPPY) {
            if (refresh_floppy_drive(dev)) changed = true;
        }
    }
    for (int index = 0; index < 26; index++) {
        vfs_drive_t* drive = &g_vfs_drives[index];
        if (!drive->present || drive->drv_type != VFS_DRV_CDROM ||
            !drive->dev) {
            continue;
        }
        if (refresh_optical_drive(drive)) changed = true;
    }
    return changed;
}

int vfs_eject_media(char letter) {
    letter = normalize_letter(letter);
    vfs_drive_t* drive = vfs_get_drive(letter);
    if (!drive || !drive->dev) {
        return VFS_ERR_NO_DRIVE;
    }
    if (drive->drv_type == VFS_DRV_FLOPPY) {
        if (!drive->dev->eject || drive->dev->eject(drive->dev) != 0) {
            return VFS_ERR_IO;
        }
        drive->media_generation = UINT32_MAX;
        (void)refresh_floppy_drive(drive->dev);
        return VFS_OK;
    }
    if (drive->drv_type != VFS_DRV_CDROM) {
        return VFS_ERR_UNSUPPORTED;
    }
    int result;
    if (drive->dev->type == BLK_TYPE_ATAPI) {
        result = strncmp(drive->dev->name, "ahci", 4) == 0
                     ? ahci_atapi_eject(drive->dev)
                     : ata_atapi_eject(drive->dev);
    } else {
        result = VFS_ERR_UNSUPPORTED;
    }
    if (result != 0) return VFS_ERR_IO;
    drive->media_generation = UINT32_MAX;
    (void)refresh_optical_drive(drive);
    return VFS_OK;
}

int vfs_eject_optical(char letter) {
    return vfs_eject_media(letter);
}

static iso_fs_t* iso_for_drive(vfs_drive_t* drive) {
    if (!drive || !drive->dev) return NULL;
    for (int index = 0; index < 4; index++) {
        if (g_iso_filesystems[index].mounted &&
            g_iso_filesystems[index].dev == drive->dev) {
            return &g_iso_filesystems[index];
        }
    }
    return NULL;
}

static const char* path_without_drive(const char* path) {
    if (!path) return NULL;
    if (path[0] && path[1] == ':') {
        path += 2;
    }
    while (*path == '\\' || *path == '/') path++;
    return path;
}

struct vfs_list_bridge {
    void (*user_callback)(const vfs_dirent_t*);
};

static struct vfs_list_bridge g_list_bridge;

static void fat_list_entry(const file_info_t* info) {
    vfs_dirent_t entry;
    memset(&entry, 0, sizeof(entry));
    copy_text(entry.name, sizeof(entry.name), info->name);
    entry.is_dir = (info->attr & 0x10) != 0;
    entry.size = info->size;
    entry.first_cluster = info->first_cluster;
    if (g_list_bridge.user_callback) g_list_bridge.user_callback(&entry);
}

static void exfat_list_entry(const exfat_file_info_t* info) {
    vfs_dirent_t entry;
    memset(&entry, 0, sizeof(entry));
    copy_text(entry.name, sizeof(entry.name), info->name);
    entry.is_dir = info->is_dir;
    entry.size = info->size;
    entry.first_cluster = info->first_cluster;
    if (g_list_bridge.user_callback) g_list_bridge.user_callback(&entry);
}

static void iso_list_entry(const iso_file_info_t* info) {
    vfs_dirent_t entry;
    memset(&entry, 0, sizeof(entry));
    copy_text(entry.name, sizeof(entry.name), info->name);
    entry.is_dir = info->is_dir;
    entry.size = info->size;
    entry.first_cluster = info->extent_lba;
    if (g_list_bridge.user_callback) g_list_bridge.user_callback(&entry);
}

int vfs_list_dir(const char* path, void (*callback)(const vfs_dirent_t*)) {
    if (!path) return VFS_ERR_NOT_FOUND;
    vfs_drive_t* drive = vfs_get_drive(path[0]);
    if (!drive) return VFS_ERR_NO_DRIVE;
    if (!drive->is_supported) return VFS_ERR_UNSUPPORTED;
    if (!drive->mounted) return VFS_ERR_NO_DRIVE;
    g_list_bridge.user_callback = callback;
    switch (g_filesystem_kind[drive->letter - 'A']) {
        case VFS_FS_FAT:
            return fat_list_dir(path, fat_list_entry) == 0 ? VFS_OK :
                   VFS_ERR_IO;
        case VFS_FS_EXFAT:
            return exfat_list_dir(&g_exfat_filesystems[drive->letter - 'A'],
                                  path, exfat_list_entry) == 0 ? VFS_OK :
                   VFS_ERR_IO;
        case VFS_FS_ISO: {
            iso_fs_t* iso = iso_for_drive(drive);
            if (!iso) return VFS_ERR_NO_MEDIA;
            return iso_list_dir(iso, path_without_drive(path),
                                iso_list_entry) == 0 ? VFS_OK : VFS_ERR_IO;
        }
        default:
            return VFS_ERR_UNSUPPORTED;
    }
}

int vfs_find_file(const char* path, vfs_dirent_t* out_entry) {
    if (!path || !out_entry) return VFS_ERR_NOT_FOUND;
    vfs_drive_t* drive = vfs_get_drive(path[0]);
    if (!drive || !drive->mounted) return VFS_ERR_NO_DRIVE;
    memset(out_entry, 0, sizeof(*out_entry));
    switch (g_filesystem_kind[drive->letter - 'A']) {
        case VFS_FS_FAT: {
            file_info_t info;
            if (fat_find(path, &info) != 0) return VFS_ERR_NOT_FOUND;
            copy_text(out_entry->name, sizeof(out_entry->name), info.name);
            out_entry->is_dir = (info.attr & 0x10) != 0;
            out_entry->size = info.size;
            out_entry->first_cluster = info.first_cluster;
            return VFS_OK;
        }
        case VFS_FS_EXFAT: {
            exfat_file_info_t info;
            if (exfat_find(&g_exfat_filesystems[drive->letter - 'A'], path,
                           &info) != 0) {
                return VFS_ERR_NOT_FOUND;
            }
            copy_text(out_entry->name, sizeof(out_entry->name), info.name);
            out_entry->is_dir = info.is_dir;
            out_entry->size = info.size;
            out_entry->first_cluster = info.first_cluster;
            return VFS_OK;
        }
        case VFS_FS_ISO: {
            iso_fs_t* iso = iso_for_drive(drive);
            iso_file_info_t info;
            if (!iso || iso_find_file(iso, path_without_drive(path),
                                      &info) != 0) {
                return VFS_ERR_NOT_FOUND;
            }
            copy_text(out_entry->name, sizeof(out_entry->name), info.name);
            out_entry->is_dir = info.is_dir;
            out_entry->size = info.size;
            out_entry->first_cluster = info.extent_lba;
            return VFS_OK;
        }
        default:
            return VFS_ERR_UNSUPPORTED;
    }
}

int vfs_read_file(const char* path, unsigned char* buffer,
                  unsigned int max_size) {
    if (!path || !buffer) return VFS_ERR_NOT_FOUND;
    vfs_drive_t* drive = vfs_get_drive(path[0]);
    if (!drive || !drive->mounted) return VFS_ERR_NO_DRIVE;
    switch (g_filesystem_kind[drive->letter - 'A']) {
        case VFS_FS_FAT: {
            file_handle_t* handle = fat_open(path);
            if (!handle) return VFS_ERR_NOT_FOUND;
            int read = fat_read(handle, buffer, max_size);
            fat_close(handle);
            return read;
        }
        case VFS_FS_EXFAT: {
            exfat_file_info_t info;
            if (exfat_find(&g_exfat_filesystems[drive->letter - 'A'], path,
                           &info) != 0) {
                return VFS_ERR_NOT_FOUND;
            }
            return exfat_read_file(
                &g_exfat_filesystems[drive->letter - 'A'], &info, 0, buffer,
                max_size);
        }
        case VFS_FS_ISO: {
            iso_fs_t* iso = iso_for_drive(drive);
            iso_file_info_t info;
            if (!iso || iso_find_file(iso, path_without_drive(path),
                                      &info) != 0) {
                return VFS_ERR_NOT_FOUND;
            }
            return iso_read_file(iso, &info, 0, buffer, max_size);
        }
        default:
            return VFS_ERR_UNSUPPORTED;
    }
}

int vfs_write_file(const char* path, const unsigned char* buffer,
                   unsigned int size) {
    if (!path || !buffer) return VFS_ERR_NOT_FOUND;
    vfs_drive_t* drive = vfs_get_drive(path[0]);
    if (!drive || !drive->mounted) return VFS_ERR_NO_DRIVE;
    if (drive->is_readonly) return VFS_ERR_READONLY;
    if (vfs_live_desktop_locked(path)) return VFS_ERR_READONLY;
    if (log_disk_is_protected_path(path, false)) return VFS_ERR_PROTECTED;
    if (g_filesystem_kind[drive->letter - 'A'] == VFS_FS_FAT) {
        return fat_write_file(path, buffer, size) == 0 ? VFS_OK : VFS_ERR_IO;
    }
    if (g_filesystem_kind[drive->letter - 'A'] == VFS_FS_EXFAT) {
        return exfat_write_file(&g_exfat_filesystems[drive->letter - 'A'],
                                path, buffer, size) == 0 ? VFS_OK :
               VFS_ERR_IO;
    }
    return VFS_ERR_READONLY;
}

int vfs_mkdir(const char* path) {
    if (!path) return VFS_ERR_NOT_FOUND;
    vfs_drive_t* drive = vfs_get_drive(path[0]);
    if (!drive || !drive->mounted) return VFS_ERR_NO_DRIVE;
    if (drive->is_readonly) return VFS_ERR_READONLY;
    if (vfs_live_desktop_locked(path)) return VFS_ERR_READONLY;
    if (g_filesystem_kind[drive->letter - 'A'] == VFS_FS_FAT) {
        return fat_create_dir(path) == 0 ? VFS_OK : VFS_ERR_IO;
    }
    if (g_filesystem_kind[drive->letter - 'A'] == VFS_FS_EXFAT) {
        return exfat_mkdir(&g_exfat_filesystems[drive->letter - 'A'], path) == 0
                   ? VFS_OK
                   : VFS_ERR_IO;
    }
    return VFS_ERR_UNSUPPORTED;
}

int vfs_unlink(const char* path) {
    if (!path) return VFS_ERR_NOT_FOUND;
    vfs_drive_t* drive = vfs_get_drive(path[0]);
    if (!drive || !drive->mounted) return VFS_ERR_NO_DRIVE;
    if (drive->is_readonly) return VFS_ERR_READONLY;
    if (vfs_live_desktop_locked(path)) return VFS_ERR_READONLY;
    if (vfs_path_is_protected(path)) return VFS_ERR_PROTECTED;
    if (g_filesystem_kind[drive->letter - 'A'] == VFS_FS_FAT) {
        return fat_delete(path) == 0 ? VFS_OK : VFS_ERR_IO;
    }
    if (g_filesystem_kind[drive->letter - 'A'] == VFS_FS_EXFAT) {
        return exfat_unlink(&g_exfat_filesystems[drive->letter - 'A'], path) == 0
                   ? VFS_OK
                   : VFS_ERR_IO;
    }
    return VFS_ERR_UNSUPPORTED;
}

int vfs_rename(const char* old_path, const char* new_path) {
    if (!old_path || !new_path) return VFS_ERR_NOT_FOUND;
    vfs_drive_t* drive = vfs_get_drive(old_path[0]);
    if (!drive || !drive->mounted) return VFS_ERR_NO_DRIVE;
    if (drive->is_readonly) return VFS_ERR_READONLY;
    if (vfs_live_desktop_locked(old_path) ||
        vfs_live_desktop_locked(new_path)) {
        return VFS_ERR_READONLY;
    }
    if (vfs_path_is_protected(old_path) ||
        vfs_path_is_protected(new_path)) {
        return VFS_ERR_PROTECTED;
    }
    if (g_filesystem_kind[drive->letter - 'A'] == VFS_FS_FAT) {
        return fat_rename(old_path, new_path) == 0 ? VFS_OK : VFS_ERR_IO;
    }
    if (g_filesystem_kind[drive->letter - 'A'] == VFS_FS_EXFAT) {
        return exfat_rename(&g_exfat_filesystems[drive->letter - 'A'],
                            old_path, new_path) == 0
                   ? VFS_OK
                   : VFS_ERR_IO;
    }
    return VFS_ERR_UNSUPPORTED;
}

static int remove_legacy_system_file(const char* path) {
    vfs_drive_t* drive = vfs_get_drive(path[0]);
    if (!drive || !drive->mounted || drive->is_readonly) return -1;
    if (g_filesystem_kind[drive->letter - 'A'] == VFS_FS_FAT) {
        return fat_delete(path);
    }
    if (g_filesystem_kind[drive->letter - 'A'] == VFS_FS_EXFAT) {
        return exfat_unlink(&g_exfat_filesystems[drive->letter - 'A'], path);
    }
    return -1;
}

typedef struct {
    char names[16][128];
    int count;
} legacy_demo_scan_t;

static legacy_demo_scan_t g_legacy_demo_scan;

static void collect_legacy_demo_entry(const vfs_dirent_t* entry) {
    if (!entry || entry->is_dir || g_legacy_demo_scan.count >= 16) return;
    copy_text(g_legacy_demo_scan.names[g_legacy_demo_scan.count++],
              sizeof(g_legacy_demo_scan.names[0]), entry->name);
}

void vfs_migrate_legacy_layout(void) {
    char drive = settings_get_system_drive();
    if (!drive) return;

    char legacy_puzzle[64];
    legacy_puzzle[0] = drive;
    legacy_puzzle[1] = ':';
    copy_text(legacy_puzzle + 2, sizeof(legacy_puzzle) - 2,
              "\\APPS\\puzzle.pxe");
    vfs_dirent_t entry;
    if (vfs_find_file(legacy_puzzle, &entry) == VFS_OK &&
        remove_legacy_system_file(legacy_puzzle) == 0) {
        log_printf("[VFS ] Upgrade moved legacy puzzle out of APPS\n");
    }

    char demo_dir[48];
    demo_dir[0] = drive;
    demo_dir[1] = ':';
    copy_text(demo_dir + 2, sizeof(demo_dir) - 2, "\\DEMO");
    if (vfs_find_file(demo_dir, &entry) != VFS_OK || !entry.is_dir) return;
    memset(&g_legacy_demo_scan, 0, sizeof(g_legacy_demo_scan));
    if (vfs_list_dir(demo_dir, collect_legacy_demo_entry) == VFS_OK) {
        for (int index = 0; index < g_legacy_demo_scan.count; index++) {
            char path[96];
            copy_text(path, sizeof(path), demo_dir);
            size_t used = strlen(path);
            copy_text(path + used, sizeof(path) - used, "\\");
            used = strlen(path);
            copy_text(path + used, sizeof(path) - used,
                      g_legacy_demo_scan.names[index]);
            (void)vfs_unlink(path);
        }
    }
    if (vfs_list_dir(demo_dir, NULL) == VFS_OK) {
        (void)vfs_unlink(demo_dir);
    }
    fat_flush_all();
}

int vfs_get_drive_space(char letter, uint64_t* total_kb, uint64_t* free_kb,
                        int* is_known) {
    vfs_drive_t* drive = vfs_get_drive(letter);
    if (!drive) return -1;
    if (total_kb) *total_kb = drive->total_kb;
    if (is_known) *is_known = drive->drv_type == VFS_DRV_CDROM;
    if (free_kb) *free_kb = 0;
    if (g_filesystem_kind[drive->letter - 'A'] == VFS_FS_FAT) {
        unsigned int total = 0;
        unsigned int free = 0;
        if (fat_get_volume_space(drive->letter, &total, &free) == 0) {
            if (total_kb) *total_kb = total;
            if (free_kb) *free_kb = free;
            if (is_known) *is_known = 1;
        }
    } else if (g_filesystem_kind[drive->letter - 'A'] == VFS_FS_EXFAT) {
        uint64_t total = 0;
        uint64_t free = 0;
        if (exfat_get_volume_space(&g_exfat_filesystems[drive->letter - 'A'],
                                   &total, &free) == 0) {
            if (total_kb) *total_kb = total;
            if (free_kb) *free_kb = free;
            if (is_known) *is_known = 1;
        }
    }
    return 0;
}

int vfs_stream_open(const char* path, vfs_stream_t* stream) {
    if (!path || !stream) return -1;
    memset(stream, 0, sizeof(*stream));
    copy_text(stream->path, sizeof(stream->path), path);
    vfs_dirent_t entry;
    if (vfs_find_file(path, &entry) != 0 || entry.is_dir) return -1;
    stream->size = entry.size;
    vfs_drive_t* drive = vfs_get_drive(path[0]);
    if (!drive || !drive->mounted) return -1;
    stream->fs_kind = g_filesystem_kind[drive->letter - 'A'];
    stream->is_write = 0;
    if (stream->fs_kind == VFS_FS_FAT) {
        stream->fat_handle = fat_open(path);
        if (!stream->fat_handle) return -1;
    } else if (stream->fs_kind == VFS_FS_EXFAT) {
        stream->exfat_handle =
            exfat_open(&g_exfat_filesystems[drive->letter - 'A'], path);
        if (!stream->exfat_handle) return -1;
    } else if (stream->fs_kind == VFS_FS_ISO) {
        stream->is_cdrom = 1;
        stream->cdrom_extent_lba = entry.first_cluster;
    } else {
        return -1;
    }
    stream->is_open = 1;
    stream->cached_sec_lba = UINT32_MAX;
    stream->has_cached_sec = 0;
    return 0;
}

int vfs_stream_create(const char* path, vfs_stream_t* stream) {
    if (!path || !stream) return -1;
    memset(stream, 0, sizeof(*stream));
    copy_text(stream->path, sizeof(stream->path), path);
    vfs_drive_t* drive = vfs_get_drive(path[0]);
    if (!drive || !drive->mounted || drive->is_readonly) return -1;
    if (vfs_live_desktop_locked(path)) return -1;
    if (log_disk_is_protected_path(path, false)) return VFS_ERR_PROTECTED;
    stream->fs_kind = g_filesystem_kind[drive->letter - 'A'];
    stream->is_write = 1;
    if (stream->fs_kind == VFS_FS_FAT) {
        stream->fat_handle = fat_open_write(path);
        if (!stream->fat_handle) return -1;
    } else if (stream->fs_kind == VFS_FS_EXFAT) {
        stream->exfat_handle =
            exfat_open_write(&g_exfat_filesystems[drive->letter - 'A'], path);
        if (!stream->exfat_handle) return -1;
    } else {
        return -1;
    }
    stream->is_open = 1;
    stream->cached_sec_lba = UINT32_MAX;
    stream->has_cached_sec = 0;
    return 0;
}

int vfs_stream_read(vfs_stream_t* stream, void* buffer, unsigned int size) {
    if (!stream || !stream->is_open || stream->is_write || !buffer) return -1;
    if (stream->current_offset >= stream->size) return 0;
    uint64_t remaining = stream->size - stream->current_offset;
    if (size > remaining) size = (unsigned int)remaining;
    vfs_drive_t* drive = vfs_get_drive(stream->path[0]);
    if (!drive) return -1;
    int result = -1;
    if (stream->fs_kind == VFS_FS_FAT && stream->fat_handle) {
        result = fat_read((file_handle_t*)stream->fat_handle, buffer, size);
    } else if (stream->fs_kind == VFS_FS_EXFAT && stream->exfat_handle) {
        result = exfat_read((exfat_handle_t*)stream->exfat_handle, buffer, size);
    } else if (stream->fs_kind == VFS_FS_ISO) {
        iso_fs_t* iso = iso_for_drive(drive);
        if (iso) {
            result = iso_read_extent(iso, stream->cdrom_extent_lba, stream->size,
                                     stream->current_offset, buffer, size,
                                     &stream->cached_sec_lba,
                                     &stream->has_cached_sec,
                                     stream->sec_cache);
        }
    }
    if (result > 0) stream->current_offset += (uint64_t)result;
    return result;
}

int vfs_stream_write(vfs_stream_t* stream, const void* buffer,
                     unsigned int size) {
    if (!stream || !stream->is_open || !stream->is_write ||
        (!buffer && size > 0)) {
        return -1;
    }
    if (size == 0) return 0;
    int result = -1;
    if (stream->fs_kind == VFS_FS_FAT && stream->fat_handle) {
        result = fat_write((file_handle_t*)stream->fat_handle, buffer, size);
    } else if (stream->fs_kind == VFS_FS_EXFAT && stream->exfat_handle) {
        result =
            exfat_write((exfat_handle_t*)stream->exfat_handle, buffer, size);
    }
    if (result > 0) {
        stream->current_offset += (uint64_t)result;
        if (stream->current_offset > stream->size) {
            stream->size = stream->current_offset;
        }
    }
    return result;
}

int vfs_stream_seek(vfs_stream_t* stream, uint64_t offset) {
    if (!stream || !stream->is_open || stream->is_write) return -1;
    if (offset > stream->size) offset = stream->size;
    if (stream->fat_handle) {
        if (fat_seek((file_handle_t*)stream->fat_handle, offset) != 0) {
            return -1;
        }
    } else if (stream->exfat_handle) {
        if (exfat_seek((exfat_handle_t*)stream->exfat_handle, offset) != 0) {
            return -1;
        }
    }
    stream->current_offset = offset;
    return 0;
}

int vfs_stream_sync(vfs_stream_t* stream) {
    if (!stream || !stream->is_open || !stream->is_write) return -1;
    if (stream->fat_handle) {
        return fat_sync((file_handle_t*)stream->fat_handle);
    }
    if (stream->exfat_handle) {
        return exfat_sync((exfat_handle_t*)stream->exfat_handle);
    }
    return -1;
}

void vfs_stream_close(vfs_stream_t* stream) {
    if (!stream) return;
    if (stream->fat_handle) {
        fat_close((file_handle_t*)stream->fat_handle);
        stream->fat_handle = NULL;
    }
    if (stream->exfat_handle) {
        exfat_close((exfat_handle_t*)stream->exfat_handle);
        stream->exfat_handle = NULL;
    }
    stream->is_open = 0;
}

int vfs_has_cdrom(void) {
    return vfs_get_boot_cdrom_letter() != 0;
}

static bool vfs_drive_has_file(char letter, const char* suffix) {
    char path[48];
    path[0] = letter;
    path[1] = ':';
    copy_text(path + 2, sizeof(path) - 2, suffix);
    vfs_dirent_t entry;
    return vfs_find_file(path, &entry) == 0 && !entry.is_dir;
}

static bool vfs_boot_disk_matches(blkdev_t* dev, uint32_t disk_id) {
    if (!dev || disk_id == 0) return false;
    unsigned char mbr[512];
    if (block_read(dev, 0, 1, mbr) != 0) return false;
    uint32_t signature = (uint32_t)mbr[440] |
                         ((uint32_t)mbr[441] << 8) |
                         ((uint32_t)mbr[442] << 16) |
                         ((uint32_t)mbr[443] << 24);
    return signature == disk_id;
}

char vfs_get_boot_cdrom_letter(void) {
    const struct limine_file* boot_file =
        kernel_file_request.response ?
            kernel_file_request.response->kernel_file : NULL;
    uint32_t boot_disk_id = boot_file ? boot_file->mbr_disk_id : 0;

    if (boot_disk_id != 0) {
        for (int index = 0; index < 26; index++) {
            vfs_drive_t* drive = &g_vfs_drives[index];
            if (!drive->present || !drive->mounted) continue;
            if (drive->drv_type != VFS_DRV_CDROM &&
                drive->drv_type != VFS_DRV_USB) {
                continue;
            }
            if (vfs_boot_disk_matches(drive->dev, boot_disk_id) &&
                vfs_drive_has_file(drive->letter, "\\boot\\kernel.elf")) {
                return drive->letter;
            }
        }
    }

    for (int index = 0; index < 26; index++) {
        vfs_drive_t* drive = &g_vfs_drives[index];
        if (!drive->present || drive->drv_type != VFS_DRV_CDROM ||
            !drive->mounted) {
            continue;
        }
        if (vfs_drive_has_file(drive->letter, "\\boot\\kernel.elf")) {
            return drive->letter;
        }
    }

    for (int index = 0; index < 26; index++) {
        vfs_drive_t* drive = &g_vfs_drives[index];
        if (!drive->present || drive->drv_type != VFS_DRV_USB ||
            !drive->mounted) {
            continue;
        }
        if (vfs_drive_has_file(drive->letter, "\\boot\\kernel.elf") &&
            (vfs_drive_has_file(drive->letter, "\\APPS\\files.pxe") ||
             vfs_drive_has_file(drive->letter, "\\payload\\boot\\kernel.elf"))) {
            return drive->letter;
        }
    }
    return 0;
}

int vfs_is_booted_from_cdrom(void) {
    if (settings_get_boot_mode() == SETTINGS_BOOT_LOCAL &&
        settings_get_system_drive() != 0) {
        return 0;
    }
    return vfs_get_boot_cdrom_letter() != 0;
}

int vfs_remount(char letter) {
    vfs_drive_t* drive = vfs_get_drive(letter);
    if (!drive || !drive->dev) return VFS_ERR_NO_DRIVE;
    if (drive->drv_type == VFS_DRV_FLOPPY) {
        drive->media_generation = UINT32_MAX;
        (void)refresh_floppy_drive(drive->dev);
        if (!drive->present) return VFS_ERR_NO_MEDIA;
        return drive->mounted ? VFS_OK : VFS_ERR_UNSUPPORTED;
    }
    if (drive->drv_type == VFS_DRV_CDROM) {
        drive->media_generation = UINT32_MAX;
        (void)refresh_optical_drive(drive);
        return drive->mounted ? VFS_OK : VFS_ERR_NO_MEDIA;
    }
    drive->mounted = 0;
    g_filesystem_kind[drive->letter - 'A'] = VFS_FS_NONE;
    if (drive->fs_type == PART_FS_EXFAT) {
        if (exfat_mount(drive->dev, drive->start_lba,
                        &g_exfat_filesystems[drive->letter - 'A']) != 0) {
            return VFS_ERR_IO;
        }
        g_filesystem_kind[drive->letter - 'A'] = VFS_FS_EXFAT;
    } else if (drive->fs_type == PART_FS_FAT12 ||
               drive->fs_type == PART_FS_FAT16 ||
               drive->fs_type == PART_FS_FAT32) {
        if (fat_mount_dev(drive->letter, drive->dev, drive->start_lba) != 0) {
            return VFS_ERR_IO;
        }
        g_filesystem_kind[drive->letter - 'A'] = VFS_FS_FAT;
    } else {
        return VFS_ERR_UNSUPPORTED;
    }
    drive->is_supported = 1;
    drive->mounted = 1;
    return VFS_OK;
}
