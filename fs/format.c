#include "format.h"
#include "fat.h"
#include "log.h"
#include "string.h"

#define FORMAT_SECTOR_SIZE 512u
#define FORMAT_ZERO_BATCH 32u

static uint16_t get_le16(const uint8_t* data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t get_le32(const uint8_t* data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void put_le16(uint8_t* data, uint16_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t* data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static int parse_label11(const uint8_t* raw, char out11[11]) {
    int has_nonspace = 0;
    for (int i = 0; i < 11; i++) {
        uint8_t c = raw[i];
        if (c == 0) {
            for (int j = i; j < 11; j++) out11[j] = ' ';
            break;
        }
        if (c < 0x20 || c >= 0x7F) return 0;
        out11[i] = (char)c;
        if (c != ' ') has_nonspace = 1;
    }
    if (!has_nonspace) return 0;
    if (memcmp(out11, "NO NAME    ", 11) == 0) return 0;
    return 1;
}

static int scan_fat_dir_label(block_device_t* dev, uint64_t lba,
                              uint32_t sectors, char out11[11]) {
    uint8_t sec[FORMAT_SECTOR_SIZE];
    for (uint32_t s = 0; s < sectors; s++) {
        if (block_read(dev, lba + s, 1, sec) != 0) return 0;
        for (int i = 0; i < 16; i++) {
            const uint8_t* ent = &sec[i * 32];
            if (ent[0] == 0x00) return 0;
            if (ent[0] == 0xE5 || ent[11] == 0x0F) continue;
            if (ent[11] & 0x08) {
                if (parse_label11(ent, out11)) return 1;
            }
        }
    }
    return 0;
}

static void read_existing_volume_label(block_device_t* dev, uint64_t start_lba,
                                       uint64_t sector_count,
                                       char out_label11[11],
                                       int* out_has_label) {
    memcpy(out_label11, "NO NAME    ", 11);
    *out_has_label = 0;
    if (!dev || !dev->read_sectors || sector_count == 0) return;

    uint8_t boot[FORMAT_SECTOR_SIZE];
    if (block_read(dev, start_lba, 1, boot) != 0) return;
    if (boot[510] != 0x55 || boot[511] != 0xAA) return;

    if (memcmp(boot + 3, "EXFAT   ", 8) == 0) {
        uint8_t bps_shift = boot[108];
        uint8_t spc_shift = boot[109];
        uint32_t heap_off = get_le32(boot + 88);
        uint32_t root_clus = get_le32(boot + 96);
        if (bps_shift == 9 && spc_shift <= 12 && root_clus >= 2 && heap_off > 0) {
            uint64_t rel = (uint64_t)heap_off + ((uint64_t)(root_clus - 2) << spc_shift);
            uint32_t spc = 1u << spc_shift;
            if (spc > 64) spc = 64;
            uint8_t sec[FORMAT_SECTOR_SIZE];
            for (uint32_t s = 0; s < spc && rel + s < sector_count; s++) {
                if (block_read(dev, start_lba + rel + s, 1, sec) != 0) break;
                for (int i = 0; i < 16; i++) {
                    const uint8_t* ent = &sec[i * 32];
                    if (ent[0] == 0x00) return;
                    if (ent[0] == 0x83) {
                        int n = ent[1];
                        if (n > 11) n = 11;
                        uint8_t raw[11];
                        memset(raw, ' ', sizeof(raw));
                        int ok = 1;
                        for (int k = 0; k < n; k++) {
                            uint16_t ch = get_le16(ent + 2 + k * 2);
                            if (ch < 0x20 || ch >= 0x7F) {
                                ok = 0;
                                break;
                            }
                            raw[k] = (uint8_t)ch;
                        }
                        if (ok && parse_label11(raw, out_label11)) {
                            *out_has_label = 1;
                        }
                        return;
                    }
                }
            }
        }
        return;
    }

    uint16_t bps = get_le16(boot + 11);
    uint8_t spc = boot[13];
    uint16_t reserved = get_le16(boot + 14);
    uint8_t num_fats = boot[16];
    uint16_t root_entries = get_le16(boot + 17);
    uint16_t fat_sz16 = get_le16(boot + 22);
    if (bps != FORMAT_SECTOR_SIZE || spc == 0 || reserved == 0 || num_fats == 0) return;

    if (root_entries == 0 && fat_sz16 == 0) {
        uint32_t fat_sz32 = get_le32(boot + 36);
        uint32_t root_clus = get_le32(boot + 44);
        uint64_t data_rel = (uint64_t)reserved + (uint64_t)num_fats * fat_sz32;
        if (fat_sz32 > 0 && root_clus >= 2 && data_rel < sector_count) {
            uint64_t root_rel = data_rel + (uint64_t)(root_clus - 2) * spc;
            if (root_rel < sector_count) {
                uint32_t scan_secs = spc;
                if (root_rel + scan_secs > sector_count) {
                    scan_secs = (uint32_t)(sector_count - root_rel);
                }
                if (scan_secs > 64) scan_secs = 64;
                if (scan_fat_dir_label(dev, start_lba + root_rel, scan_secs, out_label11)) {
                    *out_has_label = 1;
                    return;
                }
            }
        }
        if (boot[66] == 0x29 && parse_label11(boot + 71, out_label11)) {
            *out_has_label = 1;
        }
        return;
    }

    if (root_entries > 0 && fat_sz16 > 0) {
        uint64_t root_rel = (uint64_t)reserved + (uint64_t)num_fats * fat_sz16;
        uint32_t root_secs = ((uint32_t)root_entries * 32u + FORMAT_SECTOR_SIZE - 1u) / FORMAT_SECTOR_SIZE;
        if (root_rel < sector_count) {
            if (root_rel + root_secs > sector_count) {
                root_secs = (uint32_t)(sector_count - root_rel);
            }
            if (scan_fat_dir_label(dev, start_lba + root_rel, root_secs, out_label11)) {
                *out_has_label = 1;
                return;
            }
        }
        if (boot[38] == 0x29 && parse_label11(boot + 43, out_label11)) {
            *out_has_label = 1;
        }
    }
}

static int write_root_label_entry(block_device_t* dev, uint64_t root_lba,
                                  const char label11[11]) {
    uint8_t sec[FORMAT_SECTOR_SIZE];
    memset(sec, 0, sizeof(sec));
    memcpy(sec, label11, 11);
    sec[11] = 0x08;
    return block_write(dev, root_lba, 1, sec);
}

static int zero_range(block_device_t* dev, uint64_t lba, uint64_t count) {
    uint8_t zeroes[FORMAT_SECTOR_SIZE * FORMAT_ZERO_BATCH];
    memset(zeroes, 0, sizeof(zeroes));
    while (count) {
        uint32_t batch =
            count > FORMAT_ZERO_BATCH ? FORMAT_ZERO_BATCH : (uint32_t)count;
        if (block_write(dev, lba, batch, zeroes) != 0) return -1;
        lba += batch;
        count -= batch;
    }
    return 0;
}

static uint32_t pick_sectors_per_cluster(uint64_t sectors,
                                         part_fs_type_t type) {
    if (type == PART_FS_FAT16) {
        if (sectors < 32768) return 2;
        if (sectors < 65536) return 4;
        if (sectors < 131072) return 8;
        if (sectors < 262144) return 16;
        if (sectors < 524288) return 32;
        return 64;
    }
    uint64_t bytes = sectors * FORMAT_SECTOR_SIZE;
    if (bytes <= 260 * 1024ull * 1024ull) return 1;
    if (bytes <= 8ull * 1024 * 1024 * 1024) return 8;
    if (bytes <= 32ull * 1024 * 1024 * 1024) return 16;
    return 32;
}

static int write_fat32(uint64_t sectors, uint32_t hidden,
                       block_device_t* dev, uint64_t start,
                       uint32_t sectors_per_cluster,
                       const char label11[11], int has_label) {
    uint32_t reserved = 32;
    uint32_t root_entries = 0;
    uint32_t fat_size = 1;
    uint32_t clusters = 0;
    for (int pass = 0; pass < 8; pass++) {
        uint64_t data_sectors =
            sectors - reserved - 2ull * fat_size;
        clusters = (uint32_t)(data_sectors / sectors_per_cluster);
        uint64_t next = ((uint64_t)(clusters + 2) * 4 +
                         FORMAT_SECTOR_SIZE - 1) / FORMAT_SECTOR_SIZE;
        if (next == fat_size) break;
        fat_size = (uint32_t)next;
    }
    if (clusters < 65525 || clusters >= 0x0FFFFFF5u) {
        log_printf("[FORMAT] FAT32 cluster count out of range: %u\n", clusters);
        return -1;
    }

    uint8_t boot[FORMAT_SECTOR_SIZE];
    memset(boot, 0, sizeof(boot));
    boot[0] = 0xEB;
    boot[1] = 0x58;
    boot[2] = 0x90;
    memcpy(boot + 3, "MSWIN4.1", 8);
    put_le16(boot + 11, FORMAT_SECTOR_SIZE);
    boot[13] = (uint8_t)sectors_per_cluster;
    put_le16(boot + 14, (uint16_t)reserved);
    boot[16] = 2;
    put_le16(boot + 17, (uint16_t)root_entries);
    put_le16(boot + 19, sectors < 65536 ? (uint16_t)sectors : 0);
    boot[21] = 0xF8;
    put_le16(boot + 22, 0);
    put_le16(boot + 24, 63);
    put_le16(boot + 26, 255);
    put_le32(boot + 28, hidden);
    put_le32(boot + 32, sectors < 65536 ? 0 : (uint32_t)sectors);
    put_le32(boot + 36, fat_size);
    put_le16(boot + 40, 0);
    put_le16(boot + 42, 0);
    put_le32(boot + 44, 2);
    put_le16(boot + 48, 1);
    put_le16(boot + 50, 6);
    boot[64] = 0x80;
    boot[66] = 0x29;
    put_le32(boot + 67, 0x504E4F53);
    memcpy(boot + 71, label11, 11);
    memcpy(boot + 82, "FAT32   ", 8);
    boot[510] = 0x55;
    boot[511] = 0xAA;
    if (block_write(dev, start, 1, boot) != 0 ||
        block_write(dev, start + 6, 1, boot) != 0) {
        return -1;
    }

    uint8_t fsinfo[FORMAT_SECTOR_SIZE];
    memset(fsinfo, 0, sizeof(fsinfo));
    put_le32(fsinfo + 0, 0x41615252);
    put_le32(fsinfo + 484, 0x61417272);
    put_le32(fsinfo + 488, clusters - 2);
    put_le32(fsinfo + 492, 3);
    put_le32(fsinfo + 508, 0xAA550000);
    if (block_write(dev, start + 1, 1, fsinfo) != 0 ||
        block_write(dev, start + 7, 1, fsinfo) != 0) {
        return -1;
    }

    if (zero_range(dev, start + reserved,
                   (uint64_t)fat_size * 2) != 0) {
        return -1;
    }
    uint8_t fat_head[FORMAT_SECTOR_SIZE];
    memset(fat_head, 0, sizeof(fat_head));
    put_le32(fat_head, 0x0FFFFFF8);
    put_le32(fat_head + 4, 0x0FFFFFFF);
    put_le32(fat_head + 8, 0x0FFFFFFF);
    if (block_write(dev, start + reserved, 1, fat_head) != 0 ||
        block_write(dev, start + reserved + fat_size, 1, fat_head) != 0) {
        return -1;
    }

    uint64_t root = start + reserved + 2ull * fat_size;
    if (zero_range(dev, root, sectors_per_cluster) != 0) return -1;
    if (has_label && write_root_label_entry(dev, root, label11) != 0) return -1;
    log_printf("[FORMAT] FAT32 complete: %u clusters, %u sectors/cluster\n",
               clusters, sectors_per_cluster);
    return 0;
}

static int write_fat16(uint64_t sectors, uint32_t hidden,
                       block_device_t* dev, uint64_t start,
                       uint32_t sectors_per_cluster,
                       const char label11[11], int has_label) {
    const uint32_t reserved = 1;
    const uint32_t root_entries = 512;
    uint32_t root_sectors = (root_entries * 32u + 511u) / 512u;
    uint32_t fat_size = 1;
    uint32_t clusters = 0;
    for (int pass = 0; pass < 8; pass++) {
        uint64_t data_sectors =
            sectors - reserved - 2ull * fat_size - root_sectors;
        clusters = (uint32_t)(data_sectors / sectors_per_cluster);
        uint64_t next = ((uint64_t)(clusters + 2) * 2 +
                         FORMAT_SECTOR_SIZE - 1) / FORMAT_SECTOR_SIZE;
        if (next == fat_size) break;
        fat_size = (uint32_t)next;
    }
    if (clusters < 4085 || clusters >= 65525) {
        log_printf("[FORMAT] FAT16 cluster count out of range: %u\n", clusters);
        return -1;
    }

    uint8_t boot[FORMAT_SECTOR_SIZE];
    memset(boot, 0, sizeof(boot));
    boot[0] = 0xEB;
    boot[1] = 0x3C;
    boot[2] = 0x90;
    memcpy(boot + 3, "MSWIN4.1", 8);
    put_le16(boot + 11, FORMAT_SECTOR_SIZE);
    boot[13] = (uint8_t)sectors_per_cluster;
    put_le16(boot + 14, (uint16_t)reserved);
    boot[16] = 2;
    put_le16(boot + 17, (uint16_t)root_entries);
    put_le16(boot + 19, sectors < 65536 ? (uint16_t)sectors : 0);
    boot[21] = 0xF8;
    put_le16(boot + 22, (uint16_t)fat_size);
    put_le16(boot + 24, 63);
    put_le16(boot + 26, 255);
    put_le32(boot + 28, hidden);
    put_le32(boot + 32, sectors < 65536 ? 0 : (uint32_t)sectors);
    boot[36] = 0x80;
    boot[38] = 0x29;
    put_le32(boot + 39, 0x504E4F53);
    memcpy(boot + 43, label11, 11);
    memcpy(boot + 54, "FAT16   ", 8);
    boot[510] = 0x55;
    boot[511] = 0xAA;
    if (block_write(dev, start, 1, boot) != 0) return -1;

    if (zero_range(dev, start + reserved,
                   (uint64_t)fat_size * 2 + root_sectors) != 0) {
        return -1;
    }
    uint8_t fat_head[FORMAT_SECTOR_SIZE];
    memset(fat_head, 0, sizeof(fat_head));
    put_le16(fat_head, 0xFFF8);
    put_le16(fat_head + 2, 0xFFFF);
    if (block_write(dev, start + reserved, 1, fat_head) != 0 ||
        block_write(dev, start + reserved + fat_size, 1, fat_head) != 0) {
        return -1;
    }
    if (has_label) {
        uint64_t root_lba = start + reserved + 2ull * fat_size;
        if (write_root_label_entry(dev, root_lba, label11) != 0) return -1;
    }
    log_printf("[FORMAT] FAT16 complete: %u clusters, %u sectors/cluster\n",
               clusters, sectors_per_cluster);
    return 0;
}

int format_partition(block_device_t* dev, uint64_t start_lba,
                     uint64_t sector_count, part_fs_type_t type) {
    if (!dev || !dev->write_sectors || dev->is_readonly ||
        dev->sector_size != FORMAT_SECTOR_SIZE || !sector_count) {
        return -1;
    }
    if (start_lba >= dev->total_sectors ||
        sector_count > dev->total_sectors - start_lba) {
        return -1;
    }
    if (sector_count > UINT32_MAX) sector_count = UINT32_MAX;
    char label11[11];
    int has_label = 0;
    read_existing_volume_label(dev, start_lba, sector_count, label11, &has_label);
    uint32_t sectors_per_cluster =
        pick_sectors_per_cluster(sector_count, type);
    log_printf("[FORMAT] Formatting LBA %llu, %llu sectors as %s\n",
               (unsigned long long)start_lba,
               (unsigned long long)sector_count,
               type == PART_FS_FAT32 ? "FAT32" : "FAT16");
    if (type == PART_FS_FAT32) {
        return write_fat32(sector_count, (uint32_t)start_lba, dev, start_lba,
                           sectors_per_cluster, label11, has_label);
    }
    if (type == PART_FS_FAT16) {
        return write_fat16(sector_count, (uint32_t)start_lba, dev, start_lba,
                           sectors_per_cluster, label11, has_label);
    }
    return -1;
}
