#include "part.h"
#include "log.h"
#include "string.h"

partition_entry_t g_partitions[MAX_PARTITIONS];
int g_partition_count;

static uint16_t read_le16(const uint8_t* data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t* data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint64_t read_le64(const uint8_t* data) {
    return (uint64_t)read_le32(data) |
           ((uint64_t)read_le32(data + 4) << 32);
}

static void copy_trimmed(char* target, unsigned int capacity,
                         const uint8_t* source, unsigned int length) {
    unsigned int start = 0;
    unsigned int end = length;
    while (start < end && (source[start] == ' ' || source[start] == 0)) {
        start++;
    }
    while (end > start && (source[end - 1] == ' ' || source[end - 1] == 0)) {
        end--;
    }
    unsigned int out = 0;
    while (start < end && out + 1 < capacity) {
        target[out++] = (char)source[start++];
    }
    target[out] = '\0';
}

static bool guid_is_efi_system(const uint8_t* guid) {
    static const uint8_t efi_guid[16] = {
        0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
        0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
    };
    return memcmp(guid, efi_guid, sizeof(efi_guid)) == 0;
}

static void probe_filesystem(blkdev_t* dev, uint64_t lba,
                             partition_entry_t* part) {
    uint8_t sector[512];
    part->fs_type = PART_FS_UNKNOWN;
    part->is_supported = 0;
    part->label[0] = '\0';
    if (!dev || dev->sector_size < sizeof(sector) ||
        block_read(dev, lba, 1, sector) != 0) {
        return;
    }

    if (memcmp(sector + 3, "NTFS    ", 8) == 0) {
        part->fs_type = PART_FS_NTFS;
        copy_trimmed(part->label, sizeof(part->label), sector + 3, 11);
        return;
    }
    if (memcmp(sector + 3, "EXFAT   ", 8) == 0) {
        part->fs_type = PART_FS_EXFAT;
        part->is_supported = 1;
        copy_trimmed(part->label, sizeof(part->label), sector + 3, 11);
        return;
    }

    uint16_t bytes_per_sector = read_le16(sector + 11);
    uint8_t sectors_per_cluster = sector[13];
    uint16_t reserved = read_le16(sector + 14);
    uint8_t fat_count = sector[16];
    uint16_t root_entries = read_le16(sector + 17);
    uint16_t total16 = read_le16(sector + 19);
    uint16_t fat16 = read_le16(sector + 22);
    uint32_t total32 = read_le32(sector + 32);
    uint32_t fat32 = read_le32(sector + 36);
    uint32_t total = total16 ? total16 : total32;
    uint32_t fat_size = fat16 ? fat16 : fat32;

    if (bytes_per_sector == 512 && sectors_per_cluster &&
        reserved && fat_count >= 1 && fat_count <= 4 && fat_size) {
        uint32_t root_sectors = (root_entries * 32u + 511u) / 512u;
        uint32_t metadata = reserved + fat_count * fat_size + root_sectors;
        if (total > metadata) {
            uint32_t clusters = (total - metadata) / sectors_per_cluster;
            if (clusters < 4085) part->fs_type = PART_FS_FAT12;
            else if (clusters < 65525) part->fs_type = PART_FS_FAT16;
            else part->fs_type = PART_FS_FAT32;
            part->is_supported = 1;
            if (part->fs_type == PART_FS_FAT32) {
                copy_trimmed(part->label, sizeof(part->label), sector + 71,
                             11);
            } else {
                copy_trimmed(part->label, sizeof(part->label), sector + 43,
                             11);
            }
            return;
        }
    }

    if (part->sys_id == 0x83) {
        part->fs_type = PART_FS_EXT;
        part->label[0] = '\0';
    }
}

static bool add_partition(blkdev_t* dev, uint64_t start, uint64_t sectors,
                          uint8_t system_id, partition_entry_t** result) {
    if (!sectors || g_partition_count >= MAX_PARTITIONS) return false;
    partition_entry_t* part = &g_partitions[g_partition_count++];
    memset(part, 0, sizeof(*part));
    part->dev = dev;
    part->start_lba = start;
    part->total_sectors = sectors;
    part->sys_id = system_id;
    probe_filesystem(dev, start, part);
    if (result) *result = part;
    return true;
}

static void scan_gpt(blkdev_t* dev) {
    uint8_t header[512];
    if (block_read(dev, 1, 1, header) != 0 ||
        memcmp(header, "EFI PART", 8) != 0) {
        return;
    }
    uint64_t entries_lba = read_le64(header + 72);
    uint32_t entry_count = read_le32(header + 80);
    uint32_t entry_size = read_le32(header + 84);
    if (!entries_lba || !entry_count || entry_size < 128 ||
        entry_count > 256) {
        return;
    }

    uint32_t per_sector = 512u / entry_size;
    if (!per_sector) return;
    uint32_t sectors = (entry_count + per_sector - 1) / per_sector;
    uint8_t sector[512];
    for (uint32_t s = 0; s < sectors; s++) {
        if (block_read(dev, entries_lba + s, 1, sector) != 0) break;
        for (uint32_t e = 0; e < per_sector; e++) {
            uint32_t index = s * per_sector + e;
            if (index >= entry_count) break;
            uint8_t* entry = sector + e * entry_size;
            bool unused = true;
            for (int i = 0; i < 16; i++) {
                if (entry[i]) {
                    unused = false;
                    break;
                }
            }
            if (unused) continue;
            uint64_t first = read_le64(entry + 32);
            uint64_t last = read_le64(entry + 40);
            if (!first || last < first) continue;
            partition_entry_t* part = NULL;
            if (!add_partition(dev, first, last - first + 1, 0xEE, &part)) {
                return;
            }
            memcpy(part->gpt_type_guid, entry, 16);
            part->is_efi_system = guid_is_efi_system(entry);
            int out = 0;
            for (int c = 0; c < 36 && out + 1 < (int)sizeof(part->label);
                 c++) {
                uint16_t ch = read_le16(entry + 56 + c * 2);
                if (!ch) break;
                part->label[out++] = ch < 128 ? (char)ch : '_';
            }
            part->label[out] = '\0';
            if (!part->label[0]) {
                const char* label = part->is_efi_system ? "EFI System" :
                                    part->is_supported ? "GPT Volume" :
                                    "Unsupported GPT";
                memcpy(part->label, label, strlen(label) + 1);
            }
        }
    }
}

static void scan_mbr(blkdev_t* dev, const uint8_t* mbr) {
    uint64_t extended_lba = 0;
    for (int index = 0; index < 4; index++) {
        const uint8_t* entry = mbr + 0x1BE + index * 16;
        uint8_t type = entry[4];
        uint32_t start = read_le32(entry + 8);
        uint32_t sectors = read_le32(entry + 12);
        if (type == 0 || sectors == 0) continue;
        if (type == 0xEE) {
            scan_gpt(dev);
            return;
        }
        if (type == 0x05 || type == 0x0F || type == 0x85) {
            extended_lba = start;
            continue;
        }
        partition_entry_t* part = NULL;
        if (add_partition(dev, start, sectors, type, &part)) {
            part->is_efi_system = type == 0xEF;
        }
    }

    uint64_t current = extended_lba;
    for (int depth = 0; current && depth < 32; depth++) {
        uint8_t ebr[512];
        if (block_read(dev, current, 1, ebr) != 0 ||
            ebr[510] != 0x55 || ebr[511] != 0xAA) {
            break;
        }
        const uint8_t* logical = ebr + 0x1BE;
        uint8_t type = logical[4];
        uint32_t relative = read_le32(logical + 8);
        uint32_t sectors = read_le32(logical + 12);
        if (type && sectors) {
            add_partition(dev, current + relative, sectors, type, NULL);
        }
        const uint8_t* next = ebr + 0x1CE;
        uint8_t next_type = next[4];
        uint32_t next_relative = read_le32(next + 8);
        if (!next_type || !next_relative) break;
        current = extended_lba + next_relative;
    }
}

static void scan_device(blkdev_t* dev) {
    if (!dev || dev->is_readonly || dev->sector_size != 512) return;
    uint8_t mbr[512];
    if (block_read(dev, 0, 1, mbr) != 0) return;
    bool has_signature = mbr[510] == 0x55 && mbr[511] == 0xAA;
    bool has_entry = false;
    if (has_signature) {
        for (int i = 0; i < 4; i++) {
            if (mbr[0x1BE + i * 16 + 4]) {
                has_entry = true;
                break;
            }
        }
    }
    if (has_signature && has_entry) {
        scan_mbr(dev, mbr);
        return;
    }

    partition_entry_t probe;
    memset(&probe, 0, sizeof(probe));
    probe.dev = dev;
    probe.total_sectors = dev->total_sectors;
    probe.sys_id = 0x06;
    probe_filesystem(dev, 0, &probe);
    if (probe.is_supported) {
        add_partition(dev, 0, dev->total_sectors, 0x06, NULL);
    }
}

void part_scan_all(void) {
    g_partition_count = 0;
    for (int i = 0; i < blkdev_count(); i++) {
        blkdev_t* dev = blkdev_get(i);
        if (dev && (dev->type == BLK_TYPE_AHCI ||
                    dev->type == BLK_TYPE_ATA ||
                    dev->type == BLK_TYPE_USB)) {
            scan_device(dev);
        }
    }
    log_printf("[PART] Detected %d usable partition(s)\n", g_partition_count);
}

partition_entry_t* part_get(int index) {
    return index >= 0 && index < g_partition_count ? &g_partitions[index] :
           NULL;
}

partition_entry_t* part_find_by_start(blkdev_t* dev, uint64_t start_lba) {
    for (int i = 0; i < g_partition_count; i++) {
        if (g_partitions[i].dev == dev &&
            g_partitions[i].start_lba == start_lba) {
            return &g_partitions[i];
        }
    }
    return NULL;
}
