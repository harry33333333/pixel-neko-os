#include "vfs.h"
#include "exfat.h"
#include "fat.h"
#include "iso9660.h"
#include "log.h"
#include "string.h"

vfs_drive_t g_vfs_drives[26];

static iso_fs_t g_iso_filesystems[4];
static exfat_fs_t g_exfat_filesystems[26];

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
    if (!part || !part->dev || !part->is_supported) return false;
    vfs_drive_t* drive = initialize_drive(letter);
    if (!drive) return false;
    drive->drv_type = part->dev->type == BLK_TYPE_USB ? VFS_DRV_USB :
                      VFS_DRV_HDD;
    drive->fs_type = part->fs_type;
    drive->is_supported = 1;
    drive->dev = part->dev;
    drive->start_lba = part->start_lba;
    drive->total_sectors = part->total_sectors;
    drive->total_kb = part->total_sectors *
                      (part->dev->sector_size / 1024u);
    copy_text(drive->label, sizeof(drive->label), part->label);
    if (part->fs_type == PART_FS_EXFAT) {
        exfat_fs_t* exfat = &g_exfat_filesystems[letter - 'A'];
        if (exfat_mount(part->dev, part->start_lba, exfat) == 0) {
            drive->mounted = 1;
            g_filesystem_kind[letter - 'A'] = VFS_FS_EXFAT;
            if (exfat->volume_label[0]) {
                copy_text(drive->label, sizeof(drive->label),
                          exfat->volume_label);
            }
        }
    } else if (part->fs_type == PART_FS_FAT12 ||
               part->fs_type == PART_FS_FAT16 ||
               part->fs_type == PART_FS_FAT32) {
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
    return drive->mounted;
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
    for (int index = 0; index < 4; index++) {
        if (g_iso_filesystems[index].mounted &&
            g_iso_filesystems[index].dev != dev) {
            continue;
        }
        if (iso_mount(dev, &g_iso_filesystems[index]) == 0) {
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
        copy_text(drive->label, sizeof(drive->label), "Optical Drive");
    }
}

void vfs_init(void) {
    memset(g_vfs_drives, 0, sizeof(g_vfs_drives));
    memset(g_iso_filesystems, 0, sizeof(g_iso_filesystems));
    memset(g_exfat_filesystems, 0, sizeof(g_exfat_filesystems));
    memset(g_filesystem_kind, 0, sizeof(g_filesystem_kind));
    fat_init();

    int floppy_unit = 0;
    for (int index = 0; index < blkdev_count() && floppy_unit < 2; index++) {
        blkdev_t* dev = blkdev_get(index);
        if (!dev || dev->type != BLK_TYPE_FLOPPY) continue;
        char letter = (char)('A' + floppy_unit++);
        vfs_drive_t* drive = initialize_drive(letter);
        if (!drive) break;
        drive->drv_type = VFS_DRV_FLOPPY;
        drive->fs_type = PART_FS_FAT12;
        drive->is_supported = 1;
        drive->dev = dev;
        drive->total_sectors = dev->total_sectors;
        drive->total_kb = dev->total_sectors / 2;
        copy_text(drive->label, sizeof(drive->label),
                  floppy_unit == 1 ? "3.5\" Floppy (A:)" :
                                     "3.5\" Floppy (B:)");
        if (fat_mount_dev(letter, dev, 0) == 0) {
            drive->mounted = 1;
            g_filesystem_kind[letter - 'A'] = VFS_FS_FAT;
        }
    }

    char local_letter = 'C';
    char removable_letter = 'E';
    const char optical_letter = 'D';
    for (int pass = 0; pass < 2; pass++) {
        for (int index = 0; index < g_partition_count; index++) {
            partition_entry_t* part = &g_partitions[index];
            if (part->is_efi_system || !part->is_supported) continue;
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

    int mounted = 0;
    for (int index = 0; index < 26; index++) {
        if (g_vfs_drives[index].mounted) mounted++;
    }
    log_printf("[VFS ] Mounted %d drive(s); C: local, D: optical, E: removable\n",
               mounted);
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
        while (*path == '\\' || *path == '/') path++;
    }
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
    if (!drive || !drive->mounted) return VFS_ERR_NO_DRIVE;
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
    stream->is_open = 1;
    vfs_drive_t* drive = vfs_get_drive(path[0]);
    if (!drive) return -1;
    if (g_filesystem_kind[drive->letter - 'A'] == VFS_FS_FAT) {
        stream->fat_handle = fat_open(path);
        if (!stream->fat_handle) return -1;
    } else if (g_filesystem_kind[drive->letter - 'A'] == VFS_FS_ISO) {
        stream->is_cdrom = 1;
        stream->cdrom_extent_lba = entry.first_cluster;
    }
    stream->cached_sec_lba = UINT32_MAX;
    stream->has_cached_sec = 0;
    return 0;
}

int vfs_stream_read(vfs_stream_t* stream, void* buffer, unsigned int size) {
    if (!stream || !stream->is_open || !buffer) return -1;
    if (stream->current_offset >= stream->size) return 0;
    uint64_t remaining = stream->size - stream->current_offset;
    if (size > remaining) size = (unsigned int)remaining;
    vfs_drive_t* drive = vfs_get_drive(stream->path[0]);
    if (!drive) return -1;
    int result = -1;
    if (g_filesystem_kind[drive->letter - 'A'] == VFS_FS_FAT) {
        result = fat_read((file_handle_t*)stream->fat_handle, buffer, size);
    } else if (g_filesystem_kind[drive->letter - 'A'] == VFS_FS_EXFAT) {
        exfat_file_info_t info;
        if (exfat_find(&g_exfat_filesystems[drive->letter - 'A'],
                       stream->path, &info) == 0) {
            result = exfat_read_file(
                &g_exfat_filesystems[drive->letter - 'A'], &info,
                stream->current_offset, buffer, size);
        }
    } else if (g_filesystem_kind[drive->letter - 'A'] == VFS_FS_ISO) {
        iso_fs_t* iso = iso_for_drive(drive);
        iso_file_info_t info;
        if (iso && iso_find_file(iso, path_without_drive(stream->path),
                                 &info) == 0) {
            result = iso_read_file(iso, &info, stream->current_offset,
                                   buffer, size);
        }
    }
    if (result > 0) stream->current_offset += result;
    return result;
}

int vfs_stream_seek(vfs_stream_t* stream, uint64_t offset) {
    if (!stream || !stream->is_open) return -1;
    if (offset > stream->size) offset = stream->size;
    stream->current_offset = offset;
    if (stream->fat_handle) {
        fat_seek((file_handle_t*)stream->fat_handle, (unsigned int)offset);
    }
    return 0;
}

void vfs_stream_close(vfs_stream_t* stream) {
    if (!stream) return;
    if (stream->fat_handle) fat_close((file_handle_t*)stream->fat_handle);
    stream->fat_handle = NULL;
    stream->is_open = 0;
}

int vfs_has_cdrom(void) {
    return vfs_get_boot_cdrom_letter() != 0;
}

char vfs_get_boot_cdrom_letter(void) {
    for (int index = 0; index < 26; index++) {
        vfs_drive_t* drive = &g_vfs_drives[index];
        if (!drive->present || drive->drv_type != VFS_DRV_CDROM ||
            !drive->mounted) {
            continue;
        }
        char path[32];
        path[0] = drive->letter;
        path[1] = ':';
        copy_text(path + 2, sizeof(path) - 2, "\\boot\\kernel.elf");
        vfs_dirent_t entry;
        if (vfs_find_file(path, &entry) == 0) return drive->letter;
    }
    return 0;
}

int vfs_is_booted_from_cdrom(void) {
    return vfs_get_boot_cdrom_letter() != 0;
}

int vfs_remount(char letter) {
    vfs_drive_t* drive = vfs_get_drive(letter);
    if (!drive || !drive->dev) return VFS_ERR_NO_DRIVE;
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
    drive->mounted = 1;
    return VFS_OK;
}
