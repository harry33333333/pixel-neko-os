#include "p5_test.h"
#include "format.h"
#include "log.h"
#include "part.h"
#include "string.h"
#include "vfs.h"

static bool starts_with_label(const char* value, const char* prefix) {
    while (*prefix) {
        char left = *value++;
        char right = *prefix++;
        if (left >= 'a' && left <= 'z') left -= 32;
        if (right >= 'a' && right <= 'z') right -= 32;
        if (!left || left != right) return false;
    }
    return true;
}

static vfs_drive_t* find_label(const char* prefix) {
    for (int index = 0; index < 26; index++) {
        if (g_vfs_drives[index].present &&
            starts_with_label(g_vfs_drives[index].label, prefix)) {
            return &g_vfs_drives[index];
        }
    }
    return NULL;
}

static bool verify_read(vfs_drive_t* drive, const char* name,
                        const char* expected) {
    if (!drive) return false;
    char path[64];
    int position = 0;
    path[position++] = drive->letter;
    path[position++] = ':';
    path[position++] = '\\';
    for (int index = 0; name[index] && position < 63; index++) {
        path[position++] = name[index];
    }
    path[position] = '\0';
    uint8_t buffer[128];
    memset(buffer, 0, sizeof(buffer));
    int length = vfs_read_file(path, buffer, sizeof(buffer) - 1);
    bool matches = length == (int)strlen(expected) &&
                   memcmp(buffer, expected, (size_t)length) == 0;
    if (!matches) {
        log_printf("[P5 ] read mismatch %s: length=%d expected=%u\n",
                   path, length, (unsigned int)strlen(expected));
    }
    return matches;
}

static bool write_and_verify(vfs_drive_t* drive, const char* name,
                             const char* text) {
    if (!drive) return false;
    char path[64];
    int position = 0;
    path[position++] = drive->letter;
    path[position++] = ':';
    path[position++] = '\\';
    for (int index = 0; name[index] && position < 63; index++) {
        path[position++] = name[index];
    }
    path[position] = '\0';
    int result = vfs_write_file(path, (const uint8_t*)text,
                                (unsigned int)strlen(text));
    if (result != VFS_OK) {
        log_printf("[P5 ] write failed %s: result=%d\n", path, result);
        return false;
    }
    return verify_read(drive, name, text);
}

static bool format_and_verify(vfs_drive_t* drive, part_fs_type_t type,
                              const char* filename) {
    if (!drive || !drive->dev) return false;
    partition_entry_t* part = part_find_by_start(drive->dev, drive->start_lba);
    if (!part) return false;
    if (format_partition(drive->dev, drive->start_lba,
                         drive->total_sectors, type) != 0) {
        log_printf("[P5 ] format_partition failed on %c:\n", drive->letter);
        return false;
    }
    drive->fs_type = type;
    drive->is_supported = 1;
    if (vfs_remount(drive->letter) != VFS_OK) {
        log_printf("[P5 ] remount failed on %c:\n", drive->letter);
        return false;
    }
    if (!write_and_verify(drive, filename, "P5-FORMAT-OK\n")) {
        log_printf("[P5 ] formatted write/read failed on %c:\n",
                   drive->letter);
        return false;
    }
    return true;
}

int p5_run_storage_self_test(void) {
    vfs_drive_t* fat = find_label("P5DATA");
    vfs_drive_t* exfat = find_label("P5USB");
    vfs_drive_t* format16 = find_label("P5FMT16");
    vfs_drive_t* format32 = find_label("P5FMT32");
    if (!fat && !exfat && !format16 && !format32) {
        log_printf("[P5 ] Storage fixtures absent; runtime self-test skipped\n");
        return 0;
    }

    int failures = 0;
    if (fat) {
        if (verify_read(fat, "P5FAT.TXT", "P5FAT-OK\n") &&
            write_and_verify(fat, "P5WR.TXT", "P5WRITE-OK\n")) {
            log_printf("[P5 ] FAT read/write PASS on %c:\n", fat->letter);
        } else {
            log_printf("[P5 ] FAT read/write FAIL on %c:\n", fat->letter);
            failures++;
        }
    }
    if (exfat) {
        if (verify_read(exfat, "P5EXFAT.TXT", "P5EXFAT-OK\n") &&
            write_and_verify(exfat, "P5EXFAT.TXT", "P5EX-WR-OK\n")) {
            log_printf("[P5 ] exFAT read/write PASS on %c:\n", exfat->letter);
        } else {
            log_printf("[P5 ] exFAT read/write FAIL on %c:\n", exfat->letter);
            failures++;
        }
    }
    if (format16) {
        if (format_and_verify(format16, PART_FS_FAT16, "FMT16.TXT")) {
            log_printf("[P5 ] FAT16 format PASS on %c:\n", format16->letter);
        } else {
            log_printf("[P5 ] FAT16 format FAIL on %c:\n", format16->letter);
            failures++;
        }
    }
    if (format32) {
        if (format_and_verify(format32, PART_FS_FAT32, "FMT32.TXT")) {
            log_printf("[P5 ] FAT32 format PASS on %c:\n", format32->letter);
        } else {
            log_printf("[P5 ] FAT32 format FAIL on %c:\n", format32->letter);
            failures++;
        }
    }
    log_printf("[P5 ] Storage self-test %s (%d failure%s)\n",
               failures ? "FAILED" : "PASS", failures,
               failures == 1 ? "" : "s");
    return failures ? -1 : 0;
}
