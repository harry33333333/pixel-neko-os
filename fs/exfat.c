#include "exfat.h"
#include "log.h"
#include "string.h"

#define EXFAT_DIR_CACHE_SIZE (64u * 1024u)
#define EXFAT_ENTRY_FILE 0x85u
#define EXFAT_ENTRY_STREAM 0xC0u
#define EXFAT_ENTRY_NAME 0xC1u
#define EXFAT_ENTRY_BITMAP 0x81u
#define EXFAT_ENTRY_UPCASE 0x82u
#define EXFAT_ENTRY_LABEL 0x83u

static uint8_t g_exfat_directory[EXFAT_DIR_CACHE_SIZE]
    __attribute__((aligned(16)));

static uint16_t le16(const uint8_t* data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t le32(const uint8_t* data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint64_t le64(const uint8_t* data) {
    return (uint64_t)le32(data) | ((uint64_t)le32(data + 4) << 32);
}

static void put_le32(uint8_t* data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static void put_le64(uint8_t* data, uint64_t value) {
    put_le32(data, (uint32_t)value);
    put_le32(data + 4, (uint32_t)(value >> 32));
}

static int exfat_strlen(const char* text) {
    int length = 0;
    while (text && text[length]) length++;
    return length;
}

static void exfat_strcpy(char* target, const char* source, int capacity) {
    int index = 0;
    if (!target || capacity <= 0) return;
    while (source && source[index] && index + 1 < capacity) {
        target[index] = source[index];
        index++;
    }
    target[index] = '\0';
}

static char exfat_fold_ascii(char value) {
    return value >= 'a' && value <= 'z' ? (char)(value - 32) : value;
}

static bool exfat_name_equal(const char* left, const char* right) {
    while (*left && *right) {
        if (exfat_fold_ascii(*left) != exfat_fold_ascii(*right)) return false;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static uint32_t checksum_rotate(uint32_t checksum, uint8_t value) {
    return ((checksum >> 1) | (checksum << 31)) + value;
}

static bool verify_boot_region(exfat_fs_t* fs, uint64_t start_lba) {
    uint8_t sector[512];
    uint32_t checksum = 0;
    for (uint32_t index = 0; index < 11; index++) {
        if (block_read(fs->dev, start_lba + index, 1, sector) != 0) {
            return false;
        }
        for (uint32_t offset = 0; offset < sizeof(sector); offset++) {
            if ((index == 0 && (offset == 106 || offset == 107 ||
                                offset == 112))) {
                continue;
            }
            checksum = checksum_rotate(checksum, sector[offset]);
        }
    }
    if (block_read(fs->dev, start_lba + 11, 1, sector) != 0) return false;
    uint32_t stored = le32(sector);
    for (uint32_t offset = 0; offset < sizeof(sector); offset += 4) {
        if (le32(sector + offset) != checksum) {
            log_printf("[EXFAT] Checksum expected=%08x stored=%08x mismatch_offset=%u\n",
                       checksum, stored, offset);
            return false;
        }
    }
    return true;
}

static uint64_t cluster_lba(exfat_fs_t* fs, uint32_t cluster) {
    if (cluster < 2 || cluster >= fs->cluster_count + 2) return UINT64_MAX;
    return fs->partition_lba + fs->cluster_heap_offset +
           (uint64_t)(cluster - 2) * fs->sectors_per_cluster;
}

int exfat_mount(blkdev_t* dev, uint64_t partition_lba, exfat_fs_t* fs) {
    if (!dev || !fs || dev->sector_size != 512) return -1;
    memset(fs, 0, sizeof(*fs));
    uint8_t boot[512];
    if (block_read(dev, partition_lba, 1, boot) != 0 ||
        memcmp(boot + 3, "EXFAT   ", 8) != 0) {
        log_printf("[EXFAT] Missing boot signature on %s\n", dev->name);
        return -1;
    }

    fs->sector_shift = boot[108];
    uint8_t sectors_per_cluster_shift = boot[109];
    fs->cluster_shift =
        (uint8_t)(fs->sector_shift + sectors_per_cluster_shift);
    if (fs->sector_shift < 9 || fs->sector_shift > 12 ||
        fs->cluster_shift > 25) {
        log_printf("[EXFAT] Invalid shift values sector=%u cluster=%u\n",
                   fs->sector_shift, sectors_per_cluster_shift);
        return -1;
    }
    fs->sector_size = 1u << fs->sector_shift;
    fs->cluster_size = 1u << fs->cluster_shift;
    fs->sectors_per_cluster = 1u << sectors_per_cluster_shift;
    fs->volume_sectors = le64(boot + 72);
    fs->fat_offset = le32(boot + 80);
    fs->fat_length = le32(boot + 84);
    fs->cluster_heap_offset = le32(boot + 88);
    fs->cluster_count = le32(boot + 92);
    fs->root_cluster = le32(boot + 96);
    fs->volume_serial = le32(boot + 100);
    fs->revision = le16(boot + 104);
    fs->dev = dev;
    fs->partition_lba = partition_lba;
    if (!fs->cluster_count || fs->root_cluster < 2 ||
        fs->fat_offset < 24 || !fs->fat_length ||
        fs->cluster_heap_offset >= fs->volume_sectors) {
        log_printf("[EXFAT] Invalid geometry fat=%u heap=%u count=%u root=%u\n",
                   fs->fat_offset, fs->cluster_heap_offset,
                   fs->cluster_count, fs->root_cluster);
        return -1;
    }
    if (!verify_boot_region(fs, partition_lba)) {
        if (!verify_boot_region(fs, partition_lba + 12)) {
            log_printf("[EXFAT] Boot-region checksum mismatch on %s\n",
                       dev->name);
            return -1;
        }
    }

    uint8_t fat_sector[512];
    if (block_read(dev, partition_lba + fs->fat_offset, 1,
                   fat_sector) != 0) {
        log_printf("[EXFAT] Failed to read FAT\n");
        return -1;
    }
    uint64_t root_lba = cluster_lba(fs, fs->root_cluster);
    if (root_lba == UINT64_MAX) {
        log_printf("[EXFAT] Invalid root cluster %u\n", fs->root_cluster);
        return -1;
    }

    int chain_guard = 0;
    uint32_t cluster = fs->root_cluster;
    while (cluster >= 2 && cluster < 0xFFFFFFF8u && chain_guard++ < 1024) {
        uint64_t lba = cluster_lba(fs, cluster);
        if (lba == UINT64_MAX) break;
        for (uint32_t sector_index = 0;
             sector_index < fs->sectors_per_cluster; sector_index++) {
            if (block_read(dev, lba + sector_index, 1, fat_sector) != 0) {
                return -1;
            }
            for (uint32_t index = 0; index < 16; index++) {
                uint8_t* entry = fat_sector + index * 32;
                if (entry[0] == EXFAT_ENTRY_BITMAP) {
                    fs->allocation_bitmap_cluster = le32(entry + 20);
                    fs->allocation_bitmap_length = le64(entry + 24);
                } else if (entry[0] == EXFAT_ENTRY_UPCASE) {
                    fs->upcase_checksum = le32(entry + 4);
                    fs->upcase_cluster = le32(entry + 20);
                    fs->upcase_length = le64(entry + 24);
                } else if (entry[0] == EXFAT_ENTRY_LABEL) {
                    int out = 0;
                    for (int character_index = 0;
                         character_index < 15 &&
                         out + 1 < (int)sizeof(fs->volume_label);
                         character_index++) {
                        uint16_t character =
                            le16(entry + 2 + character_index * 2);
                        if (!character) break;
                        fs->volume_label[out++] =
                            character < 128 ? (char)character : '_';
                    }
                    fs->volume_label[out] = '\0';
                }
            }
        }
        uint64_t fat_lba = fs->partition_lba + fs->fat_offset +
                           (uint64_t)cluster * 4u / 512u;
        uint32_t fat_offset = (cluster * 4u) & 511u;
        if (block_read(dev, fat_lba, 1, fat_sector) != 0) break;
        uint32_t next = le32(fat_sector + fat_offset) & 0x0FFFFFFFu;
        if (next >= 0xFFFFFFF8u) break;
        cluster = next;
    }

    fs->mounted = 1;
    log_printf("[EXFAT] Mounted %s: %u clusters, %u-byte clusters\n",
               dev->name, fs->cluster_count, fs->cluster_size);
    return 0;
}

static uint32_t fat_next(exfat_fs_t* fs, uint32_t cluster) {
    uint8_t sector[512];
    uint64_t lba = fs->partition_lba + fs->fat_offset +
                   (uint64_t)cluster * 4u / sizeof(sector);
    uint32_t offset = (cluster * 4u) & (sizeof(sector) - 1);
    if (block_read(fs->dev, lba, 1, sector) != 0) return 0xFFFFFFFFu;
    return le32(sector + offset) & 0x0FFFFFFFu;
}

static int load_directory(exfat_fs_t* fs, uint32_t first_cluster,
                          uint32_t* out_length) {
    if (!fs || !fs->mounted || !out_length) return -1;
    uint32_t length = 0;
    uint32_t cluster = first_cluster;
    int guard = 0;
    while (cluster >= 2 && cluster < 0xFFFFFFF8u && guard++ < 4096) {
        uint64_t lba = cluster_lba(fs, cluster);
        if (lba == UINT64_MAX) break;
        uint32_t bytes = fs->cluster_size;
        if (bytes > EXFAT_DIR_CACHE_SIZE - length) {
            bytes = EXFAT_DIR_CACHE_SIZE - length;
        }
        if (!bytes) break;
        if (block_read(fs->dev, lba, bytes / 512,
                       g_exfat_directory + length) != 0) {
            return -1;
        }
        length += bytes;
        if (length >= EXFAT_DIR_CACHE_SIZE) break;
        uint32_t next = fat_next(fs, cluster);
        if (next >= 0xFFFFFFF8u) break;
        cluster = next;
    }
    *out_length = length;
    return length ? 0 : -1;
}

typedef int (*exfat_visit_cb_t)(void* context,
                                const exfat_file_info_t* info);

struct exfat_visit_context {
    exfat_visit_cb_t callback;
    void* user;
};

static int visit_directory(exfat_fs_t* fs, uint32_t first_cluster,
                           exfat_visit_cb_t callback, void* user) {
    uint32_t length = 0;
    if (load_directory(fs, first_cluster, &length) != 0) return -1;
    uint32_t offset = 0;
    while (offset + 32 <= length) {
        uint8_t* entry = g_exfat_directory + offset;
        uint8_t type = entry[0];
        if (type == 0) break;
        if (type != EXFAT_ENTRY_FILE) {
            offset += 32;
            continue;
        }
        uint8_t secondary = entry[1] & 0x1F;
        if (!secondary || offset + (uint32_t)(secondary + 1) * 32 > length) {
            break;
        }
        uint8_t* stream = entry + 32;
        if (stream[0] != EXFAT_ENTRY_STREAM) {
            offset += (uint32_t)(secondary + 1) * 32;
            continue;
        }
        exfat_file_info_t info;
        memset(&info, 0, sizeof(info));
        info.attributes = le16(entry + 4);
        info.is_dir = (info.attributes & 0x10) != 0;
        info.no_fat_chain = (stream[1] & 0x02) != 0;
        uint8_t name_length = stream[3];
        info.first_cluster = le32(stream + 20);
        info.valid_size = le64(stream + 8);
        info.size = le64(stream + 24);
        int out = 0;
        for (uint32_t index = 1; index <= secondary && out < 255; index++) {
            uint8_t* name_entry = entry + index * 32;
            if (name_entry[0] != EXFAT_ENTRY_NAME) continue;
            for (uint32_t char_index = 0;
                 char_index < 15 && out < name_length && out < 255;
                 char_index++) {
                uint16_t character = le16(name_entry + 2 + char_index * 2);
                info.name[out++] = character < 128 ? (char)character : '_';
            }
        }
        info.name[out] = '\0';
        if (out && callback && callback(user, &info)) return 1;
        offset += (uint32_t)(secondary + 1) * 32;
    }
    return 0;
}

struct exfat_find_context {
    const char* name;
    exfat_file_info_t* info;
    bool found;
};

static int find_visitor(void* context, const exfat_file_info_t* info) {
    struct exfat_find_context* find = context;
    if (!exfat_name_equal(info->name, find->name)) return 0;
    *find->info = *info;
    find->found = true;
    return 1;
}

static const char* next_component(const char* path, char* component,
                                  int capacity) {
    while (*path == '\\' || *path == '/') path++;
    int length = 0;
    while (*path && *path != '\\' && *path != '/' &&
           length + 1 < capacity) {
        component[length++] = *path++;
    }
    component[length] = '\0';
    return path;
}

int exfat_find(exfat_fs_t* fs, const char* path, exfat_file_info_t* info) {
    if (!fs || !fs->mounted || !path || !info) return -1;
    if (path[0] && path[1] == ':') path += 2;
    uint32_t cluster = fs->root_cluster;
    if (!*path || (*path == '\\' && !path[1])) {
        memset(info, 0, sizeof(*info));
        info->first_cluster = cluster;
        info->is_dir = true;
        return 0;
    }
    char component[256];
    while (*path) {
        path = next_component(path, component, sizeof(component));
        if (!component[0]) break;
        struct exfat_find_context find = { component, info, false };
        if (visit_directory(fs, cluster, find_visitor, &find) != 1 ||
            !find.found) {
            return -1;
        }
        if (*path && !find.info->is_dir) return -1;
        cluster = (uint32_t)find.info->first_cluster;
    }
    return 0;
}

struct exfat_list_context {
    void (*callback)(const exfat_file_info_t*);
};

static int list_visitor(void* context, const exfat_file_info_t* info) {
    struct exfat_list_context* list = context;
    if (list->callback) list->callback(info);
    return 0;
}

int exfat_list_dir(exfat_fs_t* fs, const char* path,
                   void (*callback)(const exfat_file_info_t*)) {
    exfat_file_info_t directory;
    if (exfat_find(fs, path, &directory) != 0 || !directory.is_dir) return -1;
    struct exfat_list_context list = { callback };
    return visit_directory(fs, (uint32_t)directory.first_cluster,
                           list_visitor, &list);
}

static uint32_t cluster_for_offset(exfat_fs_t* fs,
                                   const exfat_file_info_t* info,
                                   uint64_t offset) {
    uint64_t cluster_index = offset / fs->cluster_size;
    uint32_t cluster = (uint32_t)info->first_cluster;
    if (info->no_fat_chain) return cluster + (uint32_t)cluster_index;
    for (uint64_t index = 0; index < cluster_index; index++) {
        cluster = fat_next(fs, cluster);
        if (cluster >= 0xFFFFFFF8u) return 0;
    }
    return cluster;
}

int exfat_read_file(exfat_fs_t* fs, const exfat_file_info_t* info,
                    uint64_t offset, void* buffer, uint32_t size) {
    if (!fs || !info || info->is_dir || !buffer) return -1;
    if (offset >= info->valid_size) return 0;
    uint64_t available = info->valid_size - offset;
    if (size > available) size = (uint32_t)available;
    uint8_t* output = (uint8_t*)buffer;
    uint32_t copied = 0;
    while (copied < size) {
        uint64_t position = offset + copied;
        uint32_t cluster = cluster_for_offset(fs, info, position);
        if (!cluster) break;
        uint64_t lba = cluster_lba(fs, cluster);
        if (lba == UINT64_MAX) break;
        uint32_t in_cluster = (uint32_t)(position % fs->cluster_size);
        uint32_t sector_offset = in_cluster / 512u;
        uint32_t byte_offset = in_cluster & 511u;
        uint8_t sector[512];
        if (block_read(fs->dev, lba + sector_offset, 1, sector) != 0) break;
        uint32_t amount = 512u - byte_offset;
        if (amount > size - copied) amount = size - copied;
        memcpy(output + copied, sector + byte_offset, amount);
        copied += amount;
    }
    return (int)copied;
}

static int set_bitmap_bit(exfat_fs_t* fs, uint32_t cluster, bool allocated) {
    if (!fs->allocation_bitmap_cluster || cluster < 2) return -1;
    uint64_t bit = cluster - 2;
    uint64_t byte_offset = bit / 8u;
    if (byte_offset >= fs->allocation_bitmap_length) return -1;
    uint32_t cluster_index = (uint32_t)(byte_offset / fs->cluster_size);
    uint32_t in_cluster = (uint32_t)(byte_offset % fs->cluster_size);
    uint32_t target_cluster =
        fs->allocation_bitmap_cluster + cluster_index;
    uint64_t lba = cluster_lba(fs, target_cluster);
    if (lba == UINT64_MAX) return -1;
    uint8_t sector[512];
    uint64_t sector_lba = lba + in_cluster / 512u;
    uint32_t sector_offset = in_cluster & 511u;
    if (block_read(fs->dev, sector_lba, 1, sector) != 0) return -1;
    uint8_t mask = (uint8_t)(1u << (bit & 7u));
    if (allocated) sector[sector_offset] |= mask;
    else sector[sector_offset] &= (uint8_t)~mask;
    return block_write(fs->dev, sector_lba, 1, sector);
}

static int write_fat_entry(exfat_fs_t* fs, uint32_t cluster,
                           uint32_t value) {
    uint64_t lba = fs->partition_lba + fs->fat_offset +
                   (uint64_t)cluster * 4u / 512u;
    uint32_t offset = (cluster * 4u) & 511u;
    uint8_t sector[512];
    if (block_read(fs->dev, lba, 1, sector) != 0) return -1;
    put_le32(sector + offset, value);
    return block_write(fs->dev, lba, 1, sector);
}

int exfat_write_file(exfat_fs_t* fs, const char* path,
                     const void* buffer, uint64_t size) {
    exfat_file_info_t info;
    if (exfat_find(fs, path, &info) != 0 || info.is_dir || !buffer) return -1;

    uint64_t old_capacity =
        ((info.size + fs->cluster_size - 1) / fs->cluster_size) *
        fs->cluster_size;
    if (size > old_capacity) {
        log_printf("[EXFAT] Write exceeds allocated capacity (%llu > %llu)\n",
                   (unsigned long long)size,
                   (unsigned long long)old_capacity);
        return -1;
    }

    const uint8_t* input = (const uint8_t*)buffer;
    uint64_t written = 0;
    uint32_t cluster = (uint32_t)info.first_cluster;
    while (written < size) {
        uint64_t lba = cluster_lba(fs, cluster);
        if (lba == UINT64_MAX) return -1;
        uint32_t amount = fs->cluster_size;
        if (amount > size - written) amount = (uint32_t)(size - written);
        uint32_t sectors = (amount + 511u) / 512u;
        if (block_write(fs->dev, lba, sectors, input + written) != 0) {
            return -1;
        }
        written += amount;
        if (info.no_fat_chain) {
            cluster++;
        } else if (written < size) {
            cluster = fat_next(fs, cluster);
            if (cluster >= 0xFFFFFFF8u) return -1;
        }
    }

    uint32_t length = 0;
    if (load_directory(fs, fs->root_cluster, &length) != 0) return -1;
    uint32_t offset = 0;
    while (offset + 64 <= length) {
        uint8_t* entry = g_exfat_directory + offset;
        if (entry[0] == 0) break;
        if (entry[0] != EXFAT_ENTRY_FILE) {
            offset += 32;
            continue;
        }
        uint8_t* stream = entry + 32;
        if (stream[0] != EXFAT_ENTRY_STREAM) {
            offset += (entry[1] + 1) * 32;
            continue;
        }
        char name[256];
        int out = 0;
        for (uint32_t index = 1; index <= (entry[1] & 0x1F) && out < 255;
             index++) {
            uint8_t* name_entry = entry + index * 32;
            if (name_entry[0] != EXFAT_ENTRY_NAME) continue;
            for (uint32_t c = 0; c < 15 && out < stream[3]; c++) {
                uint16_t ch = le16(name_entry + 2 + c * 2);
                name[out++] = ch < 128 ? (char)ch : '_';
            }
        }
        name[out] = '\0';
        const char* requested = path;
        if (requested[0] && requested[1] == ':') requested += 2;
        while (*requested == '\\' || *requested == '/') requested++;
        if (exfat_name_equal(name, requested)) {
            put_le64(entry + 8, size);
            put_le64(stream + 8, size);
            put_le64(stream + 24, size);
            uint64_t directory_lba = cluster_lba(fs, fs->root_cluster);
            if (directory_lba == UINT64_MAX) return -1;
            return block_write(fs->dev,
                               directory_lba + offset / 512u, 1, entry -
                               (offset & 511u));
        }
        offset += (entry[1] + 1) * 32;
    }
    return -1;
}
