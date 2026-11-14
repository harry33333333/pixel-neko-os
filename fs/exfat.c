#include "exfat.h"
#include "log.h"
#include "string.h"

#define EXFAT_ENTRY_FILE 0x85u
#define EXFAT_ENTRY_STREAM 0xC0u
#define EXFAT_ENTRY_NAME 0xC1u
#define EXFAT_ENTRY_BITMAP 0x81u
#define EXFAT_ENTRY_UPCASE 0x82u
#define EXFAT_ENTRY_LABEL 0x83u
#define EXFAT_MAX_HANDLES 64
#define EXFAT_MAX_DIR_SCAN_CLUSTERS 65536u

static exfat_handle_t g_exfat_handles[EXFAT_MAX_HANDLES];

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

static void put_le64(uint8_t* data, uint64_t value) {
    put_le32(data, (uint32_t)value);
    put_le32(data + 4, (uint32_t)(value >> 32));
}

static int __attribute__((unused)) exfat_strlen(const char* text) {
    int length = 0;
    while (text && text[length]) length++;
    return length;
}

static void __attribute__((unused)) exfat_strcpy(char* target, const char* source, int capacity) {
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

    fs->alloc_hint = 2;
    fs->free_clusters = 0;
    fs->free_known = 0;
    fs->mounted = 1;
    log_printf("[EXFAT] Mounted %s: %u clusters, %u-byte clusters\n",
               dev->name, fs->cluster_count, fs->cluster_size);
    return 0;
}

static exfat_handle_t* exfat_alloc_handle(void) {
    for (int i = 0; i < EXFAT_MAX_HANDLES; i++) {
        if (!g_exfat_handles[i].is_open) {
            memset(&g_exfat_handles[i], 0, sizeof(g_exfat_handles[i]));
            return &g_exfat_handles[i];
        }
    }
    return NULL;
}

static uint32_t fat_next(exfat_fs_t* fs, uint32_t cluster) {
    if (cluster < 2 || cluster >= fs->cluster_count + 2) return 0xFFFFFFFFu;
    uint8_t sector[512];
    uint64_t lba = fs->partition_lba + fs->fat_offset +
                   (uint64_t)cluster * 4u / sizeof(sector);
    uint32_t offset = (cluster * 4u) & (sizeof(sector) - 1);
    if (block_read(fs->dev, lba, 1, sector) != 0) return 0xFFFFFFFFu;
    return le32(sector + offset) & 0x0FFFFFFFu;
}

static int write_fat_entry(exfat_fs_t* fs, uint32_t cluster, uint32_t value) {
    if (cluster < 2 || cluster >= fs->cluster_count + 2) return -1;
    uint64_t lba = fs->partition_lba + fs->fat_offset +
                   (uint64_t)cluster * 4u / 512u;
    uint32_t offset = (cluster * 4u) & 511u;
    uint8_t sector[512];
    if (block_read(fs->dev, lba, 1, sector) != 0) return -1;
    put_le32(sector + offset, value);
    return block_write(fs->dev, lba, 1, sector);
}

static uint64_t bitmap_sector_lba(exfat_fs_t* fs, uint64_t byte_offset,
                                  uint32_t* out_sec_byte_off) {
    if (!fs->allocation_bitmap_cluster || byte_offset >= fs->allocation_bitmap_length) {
        return UINT64_MAX;
    }
    uint32_t cluster_idx = (uint32_t)(byte_offset / fs->cluster_size);
    uint32_t in_cluster = (uint32_t)(byte_offset % fs->cluster_size);
    uint32_t cur = fs->allocation_bitmap_cluster;
    for (uint32_t i = 0; i < cluster_idx; i++) {
        uint32_t nxt = fat_next(fs, cur);
        if (nxt >= 2 && nxt < fs->cluster_count + 2) {
            cur = nxt;
        } else {
            /* Contiguous fallback when Allocation Bitmap uses NoFatChain semantics */
            cur++;
        }
    }
    uint64_t clba = cluster_lba(fs, cur);
    if (clba == UINT64_MAX) return UINT64_MAX;
    if (out_sec_byte_off) *out_sec_byte_off = in_cluster & 511u;
    return clba + (in_cluster / 512u);
}

static int set_bitmap_bit(exfat_fs_t* fs, uint32_t cluster, bool allocated) {
    if (!fs || !fs->allocation_bitmap_cluster || cluster < 2 ||
        cluster >= fs->cluster_count + 2) {
        return -1;
    }
    uint64_t bit = (uint64_t)(cluster - 2);
    uint64_t byte_offset = bit / 8u;
    uint32_t sec_off = 0;
    uint64_t slba = bitmap_sector_lba(fs, byte_offset, &sec_off);
    if (slba == UINT64_MAX) return -1;

    uint8_t sector[512];
    if (block_read(fs->dev, slba, 1, sector) != 0) return -1;
    uint8_t mask = (uint8_t)(1u << (bit & 7u));
    bool was_set = (sector[sec_off] & mask) != 0;
    if (allocated) sector[sec_off] |= mask;
    else sector[sec_off] &= (uint8_t)~mask;
    if (was_set != allocated && fs->free_known) {
        if (allocated && fs->free_clusters > 0) fs->free_clusters--;
        else if (!allocated) fs->free_clusters++;
    }
    return block_write(fs->dev, slba, 1, sector);
}

static uint32_t exfat_alloc_cluster(exfat_fs_t* fs) {
    if (!fs || !fs->mounted || !fs->allocation_bitmap_cluster || fs->cluster_count == 0) {
        return 0;
    }
    uint32_t start = fs->alloc_hint;
    if (start < 2 || start >= fs->cluster_count + 2) start = 2;

    uint8_t sector[512];
    uint64_t cached_lba = UINT64_MAX;

    for (uint32_t done = 0; done < fs->cluster_count; ) {
        uint32_t c = start + done;
        if (c >= fs->cluster_count + 2) c -= fs->cluster_count;
        uint64_t bit = (uint64_t)(c - 2);
        uint64_t byte_off = bit / 8u;
        uint32_t sec_off = 0;
        uint64_t slba = bitmap_sector_lba(fs, byte_off, &sec_off);
        if (slba == UINT64_MAX) return 0;
        if (slba != cached_lba) {
            if (block_read(fs->dev, slba, 1, sector) != 0) return 0;
            cached_lba = slba;
        }

        /* Fast skip completely occupied bytes when bit starts on byte boundary */
        if ((bit & 7u) == 0 && sector[sec_off] == 0xFFu &&
            done + 8u <= fs->cluster_count &&
            c + 8u <= fs->cluster_count + 2) {
            done += 8u;
            continue;
        }

        uint8_t mask = (uint8_t)(1u << (bit & 7u));
        if ((sector[sec_off] & mask) == 0) {
            sector[sec_off] |= mask;
            if (block_write(fs->dev, slba, 1, sector) != 0) return 0;
            if (fs->free_known && fs->free_clusters > 0) fs->free_clusters--;
            fs->alloc_hint = (c + 1 < fs->cluster_count + 2) ? (c + 1) : 2;
            return c;
        }
        done++;
    }
    return 0;
}

static void exfat_free_chain(exfat_fs_t* fs, uint32_t first_cluster,
                             bool no_fat_chain, uint64_t size_bytes, bool is_dir) {
    if (!fs || first_cluster < 2 || first_cluster >= fs->cluster_count + 2) return;
    if (no_fat_chain) {
        uint64_t nclust = (size_bytes + fs->cluster_size - 1u) / fs->cluster_size;
        if (nclust == 0 && is_dir) nclust = 1;
        for (uint64_t i = 0; i < nclust; i++) {
            uint32_t c = first_cluster + (uint32_t)i;
            if (c >= fs->cluster_count + 2) break;
            set_bitmap_bit(fs, c, false);
            if (fs->alloc_hint == 0 || c < fs->alloc_hint) fs->alloc_hint = c;
        }
        return;
    }

    uint32_t cur = first_cluster;
    uint32_t guard = 0;
    while (cur >= 2 && cur < fs->cluster_count + 2 && guard++ < fs->cluster_count) {
        uint32_t nxt = fat_next(fs, cur);
        set_bitmap_bit(fs, cur, false);
        write_fat_entry(fs, cur, 0);
        if (fs->alloc_hint == 0 || cur < fs->alloc_hint) fs->alloc_hint = cur;
        if (nxt >= 0xFFFFFFF8u || nxt < 2) break;
        cur = nxt;
    }
}

static char exfat_upper_ascii(char ch) {
    return (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
}

static uint16_t exfat_name_hash(const char* name, uint8_t name_len) {
    uint16_t hash = 0;
    for (uint8_t i = 0; i < name_len; i++) {
        uint8_t lo = (uint8_t)exfat_upper_ascii(name[i]);
        uint8_t hi = 0;
        hash = (uint16_t)(((hash & 1u) ? 0x8000u : 0u) + (hash >> 1) + lo);
        hash = (uint16_t)(((hash & 1u) ? 0x8000u : 0u) + (hash >> 1) + hi);
    }
    return hash;
}

static uint16_t exfat_entry_set_checksum(const uint8_t* entries, int slot_count) {
    uint32_t bytes = (uint32_t)slot_count * 32u;
    uint16_t checksum = 0;
    for (uint32_t i = 0; i < bytes; i++) {
        if (i == 2u || i == 3u) continue;
        checksum = (uint16_t)(((checksum & 1u) ? 0x8000u : 0u) + (checksum >> 1) + entries[i]);
    }
    return checksum;
}

/* Stateful Directory Cursor for arbitrarily sized directories & exact slot R/W */
typedef struct {
    exfat_fs_t* fs;
    uint32_t first_cluster;
    bool no_fat_chain;
    uint64_t byte_limit; /* 0 = unlimited (root/FAT chain end) */
    uint32_t cur_cidx;
    uint32_t cur_cluster;
    uint32_t last_cluster;
} exfat_dir_cur_t;

static void dir_cur_init(exfat_dir_cur_t* cur, exfat_fs_t* fs,
                         uint32_t first_cluster, bool no_fat_chain, uint64_t size_bytes) {
    cur->fs = fs;
    cur->first_cluster = first_cluster;
    cur->no_fat_chain = no_fat_chain;
    cur->byte_limit = (no_fat_chain && size_bytes > 0)
                          ? (((size_bytes + fs->cluster_size - 1u) / fs->cluster_size) * fs->cluster_size)
                          : 0;
    cur->cur_cidx = 0;
    cur->cur_cluster = first_cluster;
    cur->last_cluster = first_cluster;
}

static int dir_locate_slot(exfat_dir_cur_t* cur, uint32_t slot_idx,
                           uint64_t* out_lba, uint32_t* out_sec_off) {
    exfat_fs_t* fs = cur->fs;
    uint64_t byte_pos = (uint64_t)slot_idx * 32u;
    if (cur->byte_limit && byte_pos >= cur->byte_limit) return -1;
    uint32_t target_cidx = (uint32_t)(byte_pos / fs->cluster_size);
    if (target_cidx < cur->cur_cidx || cur->cur_cluster < 2) {
        cur->cur_cidx = 0;
        cur->cur_cluster = cur->first_cluster;
    }
    while (cur->cur_cidx < target_cidx) {
        if (cur->cur_cluster < 2 || cur->cur_cluster >= fs->cluster_count + 2) return -1;
        cur->last_cluster = cur->cur_cluster;
        if (cur->no_fat_chain) {
            cur->cur_cluster++;
        } else {
            uint32_t nxt = fat_next(fs, cur->cur_cluster);
            if (nxt >= 0xFFFFFFF8u || nxt < 2) return -1;
            cur->cur_cluster = nxt;
        }
        cur->cur_cidx++;
    }
    if (cur->cur_cluster < 2 || cur->cur_cluster >= fs->cluster_count + 2) return -1;
    cur->last_cluster = cur->cur_cluster;
    uint64_t clba = cluster_lba(fs, cur->cur_cluster);
    if (clba == UINT64_MAX) return -1;
    uint32_t in_cluster = (uint32_t)(byte_pos % fs->cluster_size);
    *out_lba = clba + (in_cluster / 512u);
    *out_sec_off = in_cluster & 511u;
    return 0;
}

static int dir_read_slots(exfat_dir_cur_t* cur, uint32_t start_slot,
                          int count, uint8_t* out_buf) {
    uint64_t cached_lba = UINT64_MAX;
    uint8_t sector[512];
    for (int i = 0; i < count; i++) {
        uint64_t slba = 0;
        uint32_t soff = 0;
        if (dir_locate_slot(cur, start_slot + (uint32_t)i, &slba, &soff) != 0) {
            return -1;
        }
        if (slba != cached_lba) {
            if (block_read(cur->fs->dev, slba, 1, sector) != 0) return -1;
            cached_lba = slba;
        }
        memcpy(out_buf + i * 32, sector + soff, 32);
    }
    return 0;
}

static int dir_write_slots(exfat_dir_cur_t* cur, uint32_t start_slot,
                           int count, const uint8_t* in_buf) {
    uint64_t cached_lba = UINT64_MAX;
    uint8_t sector[512];
    for (int i = 0; i < count; i++) {
        uint64_t slba = 0;
        uint32_t soff = 0;
        if (dir_locate_slot(cur, start_slot + (uint32_t)i, &slba, &soff) != 0) {
            return -1;
        }
        if (slba != cached_lba) {
            if (cached_lba != UINT64_MAX) {
                if (block_write(cur->fs->dev, cached_lba, 1, sector) != 0) return -1;
            }
            if (block_read(cur->fs->dev, slba, 1, sector) != 0) return -1;
            cached_lba = slba;
        }
        memcpy(sector + soff, in_buf + i * 32, 32);
    }
    if (cached_lba != UINT64_MAX) {
        if (block_write(cur->fs->dev, cached_lba, 1, sector) != 0) return -1;
    }
    return 0;
}

static int exfat_zero_cluster(exfat_fs_t* fs, uint32_t cluster) {
    uint64_t clba = cluster_lba(fs, cluster);
    if (clba == UINT64_MAX) return -1;
    uint8_t zero[512];
    memset(zero, 0, sizeof(zero));
    for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
        if (block_write(fs->dev, clba + s, 1, zero) != 0) return -1;
    }
    return 0;
}

typedef int (*exfat_visit_loc_cb_t)(void* context, const exfat_file_info_t* info,
                                    uint32_t entry_slot, uint8_t slot_count);

static int visit_directory_ex(exfat_fs_t* fs, uint32_t first_cluster,
                              bool no_fat_chain, uint64_t size_bytes,
                              exfat_visit_loc_cb_t callback, void* user) {
    if (!fs || !fs->mounted || first_cluster < 2) return -1;
    exfat_dir_cur_t cur;
    dir_cur_init(&cur, fs, first_cluster, no_fat_chain, size_bytes);

    uint32_t max_slots = (fs->cluster_size / 32u) * EXFAT_MAX_DIR_SCAN_CLUSTERS;
    uint32_t slot = 0;
    while (slot < max_slots) {
        uint8_t entry[32];
        if (dir_read_slots(&cur, slot, 1, entry) != 0) break;
        uint8_t type = entry[0];
        if (type == 0x00u) break;
        if (type != EXFAT_ENTRY_FILE) {
            slot++;
            continue;
        }
        uint8_t secondary = entry[1] & 0x1Fu;
        if (!secondary || (uint32_t)(secondary + 1) > 19u) {
            slot++;
            continue;
        }
        uint8_t raw_set[19 * 32];
        if (dir_read_slots(&cur, slot, secondary + 1, raw_set) != 0) break;
        uint8_t* stream = raw_set + 32;
        if (stream[0] != EXFAT_ENTRY_STREAM) {
            slot += (uint32_t)(secondary + 1);
            continue;
        }

        exfat_file_info_t info;
        memset(&info, 0, sizeof(info));
        info.attributes = le16(raw_set + 4);
        info.is_dir = (info.attributes & 0x10) != 0;
        info.no_fat_chain = (stream[1] & 0x02) != 0;
        uint8_t name_length = stream[3];
        info.first_cluster = le32(stream + 20);
        info.valid_size = le64(stream + 8);
        info.size = le64(stream + 24);
        int out = 0;
        for (uint32_t index = 1; index <= secondary && out < 255; index++) {
            uint8_t* name_entry = raw_set + index * 32;
            if (name_entry[0] != EXFAT_ENTRY_NAME) continue;
            for (uint32_t char_index = 0;
                 char_index < 15 && out < name_length && out < 255;
                 char_index++) {
                uint16_t character = le16(name_entry + 2 + char_index * 2);
                info.name[out++] = character < 128 ? (char)character : '_';
            }
        }
        info.name[out] = '\0';
        if (out && callback && callback(user, &info, slot, (uint8_t)(secondary + 1))) {
            return 1;
        }
        slot += (uint32_t)(secondary + 1);
    }
    return 0;
}

struct exfat_find_context {
    const char* name;
    exfat_file_info_t* info;
    uint32_t entry_slot;
    uint8_t slot_count;
    bool found;
};

static int find_visitor(void* context, const exfat_file_info_t* info,
                        uint32_t entry_slot, uint8_t slot_count) {
    struct exfat_find_context* find = context;
    if (!exfat_name_equal(info->name, find->name)) return 0;
    if (find->info) *find->info = *info;
    find->entry_slot = entry_slot;
    find->slot_count = slot_count;
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
    while (*path == '\\' || *path == '/') path++;
    uint32_t cluster = fs->root_cluster;
    bool nofat = false;
    uint64_t dir_size = 0;
    if (!*path) {
        memset(info, 0, sizeof(*info));
        info->first_cluster = cluster;
        info->is_dir = true;
        info->no_fat_chain = false;
        return 0;
    }
    char component[256];
    while (*path) {
        path = next_component(path, component, sizeof(component));
        if (!component[0]) break;
        struct exfat_find_context find = { component, info, 0, 0, false };
        int visit = visit_directory_ex(fs, cluster, nofat, dir_size, find_visitor, &find);
        if (visit < 0) {
            log_printf("[EXFAT] directory read failed for %s\n", component);
            return -1;
        }
        if (visit != 1 || !find.found) {
            return -1;
        }
        if (*path && !find.info->is_dir) return -1;
        cluster = (uint32_t)find.info->first_cluster;
        nofat = find.info->no_fat_chain;
        dir_size = find.info->size;
    }
    return 0;
}

struct exfat_list_context {
    void (*callback)(const exfat_file_info_t*);
};

static int list_visitor(void* context, const exfat_file_info_t* info,
                        uint32_t entry_slot, uint8_t slot_count) {
    (void)entry_slot; (void)slot_count;
    struct exfat_list_context* list = context;
    if (list->callback) list->callback(info);
    return 0;
}

int exfat_list_dir(exfat_fs_t* fs, const char* path,
                   void (*callback)(const exfat_file_info_t*)) {
    exfat_file_info_t directory;
    if (exfat_find(fs, path, &directory) != 0 || !directory.is_dir) return -1;
    struct exfat_list_context list = { callback };
    return visit_directory_ex(fs, (uint32_t)directory.first_cluster,
                              directory.no_fat_chain, directory.size,
                              list_visitor, &list);
}

typedef struct {
    uint32_t parent_cluster;
    bool     parent_nofat;
    uint64_t parent_size;
    uint32_t gp_cluster;
    bool     gp_nofat;
    uint64_t gp_size;
    uint32_t parent_in_gp_slot;
    bool     parent_is_root;
    char     leaf_name[256];
} exfat_parent_loc_t;

static int exfat_resolve_parent(exfat_fs_t* fs, const char* path, exfat_parent_loc_t* out) {
    if (!fs || !fs->mounted || !path || !out) return -1;
    memset(out, 0, sizeof(*out));
    if (path[0] && path[1] == ':') path += 2;
    while (*path == '\\' || *path == '/') path++;
    if (!*path) return -1;

    out->parent_cluster = fs->root_cluster;
    out->parent_nofat = false;
    out->parent_size = 0;
    out->parent_is_root = true;

    char comp[256];
    for (;;) {
        const char* next = next_component(path, comp, sizeof(comp));
        while (*next == '\\' || *next == '/') next++;
        if (!comp[0]) return -1;
        if (!*next) {
            exfat_strcpy(out->leaf_name, comp, sizeof(out->leaf_name));
            return 0;
        }
        exfat_file_info_t sub;
        struct exfat_find_context find = { comp, &sub, 0, 0, false };
        int v = visit_directory_ex(fs, out->parent_cluster, out->parent_nofat,
                                   out->parent_size, find_visitor, &find);
        if (v != 1 || !find.found || !sub.is_dir || sub.first_cluster < 2) return -1;
        out->gp_cluster = out->parent_cluster;
        out->gp_nofat = out->parent_nofat;
        out->gp_size = out->parent_size;
        out->parent_in_gp_slot = find.entry_slot;
        out->parent_is_root = false;
        out->parent_cluster = (uint32_t)sub.first_cluster;
        out->parent_nofat = sub.no_fat_chain;
        out->parent_size = sub.size;
        path = next;
    }
}

static int exfat_extend_dir(exfat_fs_t* fs, exfat_parent_loc_t* ploc,
                            uint32_t last_cluster, uint32_t* out_new_cluster) {
    uint32_t nc = exfat_alloc_cluster(fs);
    if (!nc) return -1;
    if (exfat_zero_cluster(fs, nc) != 0) {
        set_bitmap_bit(fs, nc, false);
        return -1;
    }

    if (ploc->parent_is_root) {
        write_fat_entry(fs, last_cluster, nc);
        write_fat_entry(fs, nc, 0xFFFFFFFFu);
    } else {
        uint64_t cur_bytes = ploc->parent_size ? ploc->parent_size : fs->cluster_size;
        uint32_t old_clusters = (uint32_t)((cur_bytes + fs->cluster_size - 1u) / fs->cluster_size);
        bool still_nofat = false;
        if (ploc->parent_nofat && nc == last_cluster + 1) {
            still_nofat = true;
        } else {
            if (ploc->parent_nofat) {
                for (uint32_t k = 0; k + 1 < old_clusters; k++) {
                    write_fat_entry(fs, ploc->parent_cluster + k, ploc->parent_cluster + k + 1);
                }
            }
            write_fat_entry(fs, last_cluster, nc);
            write_fat_entry(fs, nc, 0xFFFFFFFFu);
        }
        ploc->parent_nofat = still_nofat;
        ploc->parent_size = cur_bytes + fs->cluster_size;

        /* Update parent directory entry in grandparent */
        exfat_dir_cur_t gcur;
        dir_cur_init(&gcur, fs, ploc->gp_cluster, ploc->gp_nofat, ploc->gp_size);
        uint8_t raw[19 * 32];
        if (dir_read_slots(&gcur, ploc->parent_in_gp_slot, 2, raw) == 0 &&
            raw[0] == EXFAT_ENTRY_FILE && raw[32] == EXFAT_ENTRY_STREAM) {
            uint8_t sec_cnt = raw[1] & 0x1Fu;
            if (sec_cnt + 1 <= 19 &&
                dir_read_slots(&gcur, ploc->parent_in_gp_slot, sec_cnt + 1, raw) == 0) {
                raw[33] = (uint8_t)(0x01u | (still_nofat ? 0x02u : 0x00u));
                put_le64(raw + 40, ploc->parent_size);
                put_le64(raw + 56, ploc->parent_size);
                uint16_t csum = exfat_entry_set_checksum(raw, sec_cnt + 1);
                put_le16(raw + 2, csum);
                dir_write_slots(&gcur, ploc->parent_in_gp_slot, sec_cnt + 1, raw);
            }
        }
    }

    if (out_new_cluster) *out_new_cluster = nc;
    return 0;
}

static int exfat_alloc_dir_slots(exfat_fs_t* fs, exfat_parent_loc_t* ploc,
                                 int needed_slots, uint32_t* out_slot) {
    exfat_dir_cur_t cur;
    dir_cur_init(&cur, fs, ploc->parent_cluster, ploc->parent_nofat, ploc->parent_size);

    uint32_t run_start = 0;
    int run_len = 0;
    uint32_t slots_per_cluster = fs->cluster_size / 32u;
    uint32_t max_slots = slots_per_cluster * EXFAT_MAX_DIR_SCAN_CLUSTERS;
    uint32_t slot = 0;
    bool hit_end = false;

    for (slot = 0; slot < max_slots; slot++) {
        uint64_t slba = 0;
        uint32_t soff = 0;
        if (dir_locate_slot(&cur, slot, &slba, &soff) != 0) {
            /* Reached end of allocated clusters for this directory */
            uint32_t new_c = 0;
            if (exfat_extend_dir(fs, ploc, cur.last_cluster, &new_c) != 0) return -1;
            dir_cur_init(&cur, fs, ploc->parent_cluster, ploc->parent_nofat, ploc->parent_size);
            if (dir_locate_slot(&cur, slot, &slba, &soff) != 0) return -1;
        }
        uint8_t sector[512];
        if (block_read(fs->dev, slba, 1, sector) != 0) return -1;
        uint8_t type = sector[soff];
        if (type == 0x00u) {
            if (run_len == 0) run_start = slot;
            hit_end = true;
            /* Reserve needed_slots starting at run_start; extend dir if crossing cluster */
            uint32_t end_slot = run_start + (uint32_t)needed_slots - 1u;
            uint64_t elba = 0;
            uint32_t eoff = 0;
            while (dir_locate_slot(&cur, end_slot, &elba, &eoff) != 0) {
                uint32_t new_c = 0;
                if (exfat_extend_dir(fs, ploc, cur.last_cluster, &new_c) != 0) return -1;
                dir_cur_init(&cur, fs, ploc->parent_cluster, ploc->parent_nofat, ploc->parent_size);
            }
            *out_slot = run_start;
            return 0;
        }
        if ((type & 0x80u) == 0) {
            if (run_len == 0) run_start = slot;
            run_len++;
            if (run_len >= needed_slots) {
                *out_slot = run_start;
                return 0;
            }
        } else {
            run_len = 0;
        }
    }
    (void)hit_end;
    return -1;
}

static int exfat_create_entry_set(exfat_fs_t* fs, exfat_parent_loc_t* ploc,
                                  const char* name, uint16_t attrs,
                                  uint32_t first_cluster, bool nofat,
                                  uint64_t valid_size, uint64_t data_size,
                                  uint32_t* out_slot) {
    int nlen = exfat_strlen(name);
    if (nlen <= 0 || nlen > 255) return -1;
    uint8_t name_slots = (uint8_t)((nlen + 14) / 15);
    uint8_t total_slots = (uint8_t)(2 + name_slots);
    uint8_t raw[19 * 32];
    memset(raw, 0, sizeof(raw));

    /* 0x85 File Directory Entry */
    raw[0] = EXFAT_ENTRY_FILE;
    raw[1] = (uint8_t)(1 + name_slots);
    put_le16(raw + 4, attrs);

    /* 0xC0 Stream Extension Directory Entry */
    raw[32] = EXFAT_ENTRY_STREAM;
    raw[33] = (uint8_t)(0x01u | (nofat ? 0x02u : 0x00u));
    raw[35] = (uint8_t)nlen;
    put_le16(raw + 36, exfat_name_hash(name, (uint8_t)nlen));
    put_le64(raw + 40, valid_size);
    put_le32(raw + 52, first_cluster);
    put_le64(raw + 56, data_size);

    /* 0xC1 File Name Directory Entries */
    for (int i = 0; i < nlen; i++) {
        int s = 2 + (i / 15);
        int c = i % 15;
        uint8_t* ne = raw + s * 32;
        ne[0] = EXFAT_ENTRY_NAME;
        ne[1] = 0;
        put_le16(ne + 2 + c * 2, (uint16_t)(uint8_t)name[i]);
    }
    for (int s = 2; s < total_slots; s++) {
        raw[s * 32] = EXFAT_ENTRY_NAME;
    }

    uint16_t csum = exfat_entry_set_checksum(raw, total_slots);
    put_le16(raw + 2, csum);

    uint32_t slot = 0;
    if (exfat_alloc_dir_slots(fs, ploc, total_slots, &slot) != 0) return -1;

    exfat_dir_cur_t cur;
    dir_cur_init(&cur, fs, ploc->parent_cluster, ploc->parent_nofat, ploc->parent_size);
    if (dir_write_slots(&cur, slot, total_slots, raw) != 0) return -1;
    if (out_slot) *out_slot = slot;
    return 0;
}

static int exfat_mark_slots_deleted(exfat_fs_t* fs, uint32_t dir_cluster,
                                    bool dir_nofat, uint64_t dir_size,
                                    uint32_t entry_slot, uint8_t slot_count) {
    exfat_dir_cur_t cur;
    dir_cur_init(&cur, fs, dir_cluster, dir_nofat, dir_size);
    uint8_t raw[19 * 32];
    if (slot_count > 19) return -1;
    if (dir_read_slots(&cur, entry_slot, slot_count, raw) != 0) return -1;
    for (uint8_t i = 0; i < slot_count; i++) {
        raw[i * 32] &= 0x7Fu;
    }
    return dir_write_slots(&cur, entry_slot, slot_count, raw);
}

exfat_handle_t* exfat_open(exfat_fs_t* fs, const char* path) {
    if (!fs || !fs->mounted || !path) return NULL;
    exfat_file_info_t info;
    if (exfat_find(fs, path, &info) != 0 || info.is_dir) return NULL;
    exfat_handle_t* h = exfat_alloc_handle();
    if (!h) return NULL;
    h->fs = fs;
    h->size = info.size;
    h->valid_size = info.valid_size;
    h->first_cluster = info.first_cluster;
    h->current_cluster = (uint32_t)info.first_cluster;
    h->last_cluster = (uint32_t)info.first_cluster;
    h->current_offset = 0;
    h->cluster_offset = 0;
    h->no_fat_chain = info.no_fat_chain;
    h->write_mode = false;
    h->is_open = true;
    return h;
}

exfat_handle_t* exfat_open_write(exfat_fs_t* fs, const char* path) {
    if (!fs || !fs->mounted || !path) return NULL;
    exfat_parent_loc_t ploc;
    if (exfat_resolve_parent(fs, path, &ploc) != 0) return NULL;

    exfat_file_info_t existing;
    struct exfat_find_context find = { ploc.leaf_name, &existing, 0, 0, false };
    int v = visit_directory_ex(fs, ploc.parent_cluster, ploc.parent_nofat,
                               ploc.parent_size, find_visitor, &find);
    if (v == 1 && find.found) {
        if (existing.is_dir) return NULL;
        exfat_mark_slots_deleted(fs, ploc.parent_cluster, ploc.parent_nofat,
                                 ploc.parent_size, find.entry_slot, find.slot_count);
        if (existing.first_cluster >= 2) {
            exfat_free_chain(fs, (uint32_t)existing.first_cluster,
                             existing.no_fat_chain, existing.size, false);
        }
    }

    uint32_t slot = 0;
    if (exfat_create_entry_set(fs, &ploc, ploc.leaf_name, 0x20u,
                               0u, true, 0ULL, 0ULL, &slot) != 0) {
        return NULL;
    }

    exfat_handle_t* h = exfat_alloc_handle();
    if (!h) return NULL;
    h->fs = fs;
    h->size = 0;
    h->valid_size = 0;
    h->first_cluster = 0;
    h->current_cluster = 0;
    h->last_cluster = 0;
    h->current_offset = 0;
    h->cluster_offset = 0;
    h->dir_cluster = ploc.parent_cluster;
    h->dir_no_fat_chain = ploc.parent_nofat;
    h->entry_slot = slot;
    h->no_fat_chain = true;
    h->write_mode = true;
    h->is_open = true;
    return h;
}

int exfat_read(exfat_handle_t* h, void* buffer, uint32_t size) {
    if (!h || !h->is_open || !h->fs || !buffer) return -1;
    exfat_fs_t* fs = h->fs;
    if (h->current_offset >= h->valid_size) return 0;
    uint64_t available = h->valid_size - h->current_offset;
    if ((uint64_t)size > available) size = (uint32_t)available;
    if (size == 0) return 0;
    if (h->first_cluster < 2 || h->current_cluster < 2) return 0;

    uint8_t* output = (uint8_t*)buffer;
    uint32_t copied = 0;
    uint8_t sector[512];

    while (copied < size) {
        if (h->cluster_offset >= fs->cluster_size) {
            uint32_t nxt = h->no_fat_chain
                               ? (h->current_cluster + 1u)
                               : fat_next(fs, h->current_cluster);
            if (nxt >= 0xFFFFFFF8u || nxt < 2 || nxt >= fs->cluster_count + 2) break;
            h->current_cluster = nxt;
            h->cluster_offset = 0;
        }
        uint64_t clba = cluster_lba(fs, h->current_cluster);
        if (clba == UINT64_MAX) break;
        uint32_t sec_in_c = h->cluster_offset / 512u;
        uint32_t byte_off = h->cluster_offset & 511u;
        uint32_t rem = size - copied;

        if (byte_off == 0 && rem >= 512u) {
            uint32_t secs_left = fs->sectors_per_cluster - sec_in_c;
            uint32_t want_secs = rem / 512u;
            if (want_secs > secs_left) want_secs = secs_left;
            if (want_secs > 64u) want_secs = 64u;
            if (block_read(fs->dev, clba + sec_in_c, want_secs, output + copied) != 0) break;
            uint32_t chunk = want_secs * 512u;
            copied += chunk;
            h->current_offset += chunk;
            h->cluster_offset += chunk;
            continue;
        }

        if (block_read(fs->dev, clba + sec_in_c, 1, sector) != 0) break;
        uint32_t amount = 512u - byte_off;
        if (amount > rem) amount = rem;
        memcpy(output + copied, sector + byte_off, amount);
        copied += amount;
        h->current_offset += amount;
        h->cluster_offset += amount;
    }
    return (int)copied;
}

int exfat_write(exfat_handle_t* h, const void* buffer, uint32_t size) {
    if (!h || !h->is_open || !h->write_mode || !h->fs) return -1;
    if (size == 0) return 0;
    if (!buffer) return -1;
    exfat_fs_t* fs = h->fs;
    const uint8_t* input = (const uint8_t*)buffer;
    uint32_t written = 0;
    uint8_t sector[512];

    while (written < size) {
        if (h->first_cluster == 0 || h->cluster_offset >= fs->cluster_size) {
            uint32_t nc = exfat_alloc_cluster(fs);
            if (!nc) return -1;
            if (h->first_cluster == 0) {
                h->first_cluster = nc;
                h->last_cluster = nc;
                h->no_fat_chain = true;
            } else if (h->no_fat_chain && nc == h->last_cluster + 1u) {
                h->last_cluster = nc;
            } else {
                if (h->no_fat_chain) {
                    for (uint32_t c = (uint32_t)h->first_cluster; c < h->last_cluster; c++) {
                        write_fat_entry(fs, c, c + 1u);
                    }
                    h->no_fat_chain = false;
                }
                write_fat_entry(fs, h->last_cluster, nc);
                write_fat_entry(fs, nc, 0xFFFFFFFFu);
                h->last_cluster = nc;
            }
            h->current_cluster = nc;
            h->cluster_offset = 0;
        }

        uint64_t clba = cluster_lba(fs, h->current_cluster);
        if (clba == UINT64_MAX) return -1;
        uint32_t sec_in_c = h->cluster_offset / 512u;
        uint32_t byte_off = h->cluster_offset & 511u;
        uint32_t rem = size - written;

        if (byte_off == 0 && rem >= 512u) {
            uint32_t secs_left = fs->sectors_per_cluster - sec_in_c;
            uint32_t want_secs = rem / 512u;
            if (want_secs > secs_left) want_secs = secs_left;
            if (want_secs > 64u) want_secs = 64u;
            if (block_write(fs->dev, clba + sec_in_c, want_secs, input + written) != 0) return -1;
            uint32_t chunk = want_secs * 512u;
            written += chunk;
            h->current_offset += chunk;
            h->cluster_offset += chunk;
            if (h->current_offset > h->size) h->size = h->current_offset;
            if (h->size > h->valid_size) h->valid_size = h->size;
            continue;
        }

        uint32_t to_write = 512u - byte_off;
        if (to_write > rem) to_write = rem;
        if (byte_off != 0 || h->current_offset + to_write < h->size) {
            if (block_read(fs->dev, clba + sec_in_c, 1, sector) != 0) {
                memset(sector, 0, sizeof(sector));
            }
        } else {
            memset(sector, 0, sizeof(sector));
        }
        memcpy(sector + byte_off, input + written, to_write);
        if (block_write(fs->dev, clba + sec_in_c, 1, sector) != 0) return -1;
        written += to_write;
        h->current_offset += to_write;
        h->cluster_offset += to_write;
        if (h->current_offset > h->size) h->size = h->current_offset;
        if (h->size > h->valid_size) h->valid_size = h->size;
    }
    return (int)written;
}

int exfat_seek(exfat_handle_t* h, uint64_t offset) {
    if (!h || !h->is_open || !h->fs) return -1;
    exfat_fs_t* fs = h->fs;
    if (offset > h->valid_size) offset = h->valid_size;
    if (h->first_cluster < 2) {
        h->current_offset = 0;
        h->cluster_offset = 0;
        return 0;
    }

    uint64_t cur_cidx = h->current_offset / fs->cluster_size;
    uint64_t tgt_cidx = offset / fs->cluster_size;
    if (h->no_fat_chain) {
        h->current_cluster = (uint32_t)h->first_cluster + (uint32_t)tgt_cidx;
    } else {
        uint32_t cur = (uint32_t)h->first_cluster;
        uint64_t steps = tgt_cidx;
        if (offset >= h->current_offset && h->current_cluster >= 2 && tgt_cidx >= cur_cidx) {
            cur = h->current_cluster;
            steps = tgt_cidx - cur_cidx;
        }
        for (uint64_t i = 0; i < steps; i++) {
            uint32_t nxt = fat_next(fs, cur);
            if (nxt >= 0xFFFFFFF8u || nxt < 2) break;
            cur = nxt;
        }
        h->current_cluster = cur;
    }
    h->cluster_offset = (uint32_t)(offset % fs->cluster_size);
    h->current_offset = offset;
    return 0;
}

void exfat_close(exfat_handle_t* h) {
    if (!h) return;
    if (h->is_open && h->write_mode && h->fs) {
        exfat_dir_cur_t cur;
        dir_cur_init(&cur, h->fs, h->dir_cluster, h->dir_no_fat_chain, 0);
        uint8_t raw[19 * 32];
        if (dir_read_slots(&cur, h->entry_slot, 2, raw) == 0 &&
            raw[0] == EXFAT_ENTRY_FILE && raw[32] == EXFAT_ENTRY_STREAM) {
            uint8_t sec_cnt = raw[1] & 0x1Fu;
            if (sec_cnt + 1 <= 19 &&
                dir_read_slots(&cur, h->entry_slot, sec_cnt + 1, raw) == 0) {
                raw[33] = (uint8_t)(0x01u | (h->no_fat_chain ? 0x02u : 0x00u));
                put_le64(raw + 40, h->valid_size);
                put_le32(raw + 52, (uint32_t)h->first_cluster);
                put_le64(raw + 56, h->size);
                uint16_t csum = exfat_entry_set_checksum(raw, sec_cnt + 1);
                put_le16(raw + 2, csum);
                dir_write_slots(&cur, h->entry_slot, sec_cnt + 1, raw);
            }
        }
    }
    h->is_open = false;
    h->write_mode = false;
}

int exfat_read_file(exfat_fs_t* fs, const exfat_file_info_t* info,
                    uint64_t offset, void* buffer, uint32_t size) {
    if (!fs || !info || info->is_dir || !buffer) return -1;
    if (offset >= info->valid_size) return 0;
    exfat_handle_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.fs = fs;
    tmp.size = info->size;
    tmp.valid_size = info->valid_size;
    tmp.first_cluster = info->first_cluster;
    tmp.current_cluster = (uint32_t)info->first_cluster;
    tmp.no_fat_chain = info.no_fat_chain;
    tmp.is_open = true;
    if (exfat_seek(&tmp, offset) != 0) return -1;
    return exfat_read(&tmp, buffer, size);
}

int exfat_write_file(exfat_fs_t* fs, const char* path,
                     const void* buffer, uint64_t size) {
    if (!fs || !path || (!buffer && size > 0)) return -1;
    exfat_handle_t* h = exfat_open_write(fs, path);
    if (!h) return -1;

    const uint8_t* input = (const uint8_t*)buffer;
    uint64_t written = 0;
    while (written < size) {
        uint64_t rem = size - written;
        uint32_t chunk = (rem > 65536ULL) ? 65536u : (uint32_t)rem;
        int w = exfat_write(h, input + written, chunk);
        if (w <= 0) {
            exfat_close(h);
            return -1;
        }
        written += (uint64_t)w;
    }
    exfat_close(h);
    return 0;
}

int exfat_mkdir(exfat_fs_t* fs, const char* path) {
    if (!fs || !fs->mounted || !path) return -1;
    exfat_parent_loc_t ploc;
    if (exfat_resolve_parent(fs, path, &ploc) != 0) return -1;

    exfat_file_info_t existing;
    struct exfat_find_context find = { ploc.leaf_name, &existing, 0, 0, false };
    int v = visit_directory_ex(fs, ploc.parent_cluster, ploc.parent_nofat,
                               ploc.parent_size, find_visitor, &find);
    if (v == 1 && find.found) return -1;

    uint32_t nc = exfat_alloc_cluster(fs);
    if (!nc) return -1;
    if (exfat_zero_cluster(fs, nc) != 0) {
        set_bitmap_bit(fs, nc, false);
        return -1;
    }
    if (exfat_create_entry_set(fs, &ploc, ploc.leaf_name, 0x10u,
                               nc, true, fs->cluster_size, fs->cluster_size, NULL) != 0) {
        set_bitmap_bit(fs, nc, false);
        return -1;
    }
    return 0;
}

static int exfat_count_dir_items_cb(void* ctx, const exfat_file_info_t* info,
                                    uint32_t slot, uint8_t cnt) {
    (void)info; (void)slot; (void)cnt;
    int* non_empty = (int*)ctx;
    *non_empty = 1;
    return 1;
}

int exfat_unlink(exfat_fs_t* fs, const char* path) {
    if (!fs || !fs->mounted || !path) return -1;
    exfat_parent_loc_t ploc;
    if (exfat_resolve_parent(fs, path, &ploc) != 0) return -1;

    exfat_file_info_t target;
    struct exfat_find_context find = { ploc.leaf_name, &target, 0, 0, false };
    int v = visit_directory_ex(fs, ploc.parent_cluster, ploc.parent_nofat,
                               ploc.parent_size, find_visitor, &find);
    if (v != 1 || !find.found) return -1;

    if (target.is_dir && target.first_cluster >= 2) {
        int non_empty = 0;
        visit_directory_ex(fs, (uint32_t)target.first_cluster,
                           target.no_fat_chain, target.size,
                           exfat_count_dir_items_cb, &non_empty);
        if (non_empty) return -1;
    }

    if (exfat_mark_slots_deleted(fs, ploc.parent_cluster, ploc.parent_nofat,
                                 ploc.parent_size, find.entry_slot, find.slot_count) != 0) {
        return -1;
    }
    if (target.first_cluster >= 2) {
        exfat_free_chain(fs, (uint32_t)target.first_cluster,
                         target.no_fat_chain, target.size, target.is_dir);
    }
    return 0;
}

int exfat_rename(exfat_fs_t* fs, const char* old_path, const char* new_path) {
    if (!fs || !fs->mounted || !old_path || !new_path) return -1;
    exfat_parent_loc_t old_p, new_p;
    if (exfat_resolve_parent(fs, old_path, &old_p) != 0) return -1;
    if (exfat_resolve_parent(fs, new_path, &new_p) != 0) return -1;

    exfat_file_info_t old_info;
    struct exfat_find_context find_old = { old_p.leaf_name, &old_info, 0, 0, false };
    if (visit_directory_ex(fs, old_p.parent_cluster, old_p.parent_nofat,
                           old_p.parent_size, find_visitor, &find_old) != 1 ||
        !find_old.found) {
        return -1;
    }

    exfat_file_info_t dup_info;
    struct exfat_find_context find_new = { new_p.leaf_name, &dup_info, 0, 0, false };
    if (visit_directory_ex(fs, new_p.parent_cluster, new_p.parent_nofat,
                           new_p.parent_size, find_visitor, &find_new) == 1 &&
        find_new.found) {
        return -1;
    }

    if (exfat_create_entry_set(fs, &new_p, new_p.leaf_name, old_info.attributes,
                               (uint32_t)old_info.first_cluster, old_info.no_fat_chain,
                               old_info.valid_size, old_info.size, NULL) != 0) {
        return -1;
    }
    exfat_mark_slots_deleted(fs, old_p.parent_cluster, old_p.parent_nofat,
                             old_p.parent_size, find_old.entry_slot, find_old.slot_count);
    return 0;
}

int exfat_get_volume_space(exfat_fs_t* fs, uint64_t* total_kb, uint64_t* free_kb) {
    if (!fs || !fs->mounted) return -1;
    uint64_t total_bytes = (uint64_t)fs->cluster_count * (uint64_t)fs->cluster_size;
    if (total_kb) *total_kb = total_bytes / 1024ULL;

    if (!fs->free_known && fs->allocation_bitmap_cluster && fs->cluster_count > 0) {
        static const uint8_t popcnt_nibble[16] = {0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4};
        uint32_t used = 0;
        uint8_t sector[512];
        uint64_t full_bytes = fs->cluster_count / 8u;
        uint32_t rem_bits = fs->cluster_count & 7u;
        uint64_t byte_pos = 0;

        while (byte_pos < full_bytes) {
            uint32_t sec_off = 0;
            uint64_t slba = bitmap_sector_lba(fs, byte_pos, &sec_off);
            if (slba == UINT64_MAX || block_read(fs->dev, slba, 1, sector) != 0) break;
            uint64_t end_in_sec = full_bytes - byte_pos;
            if (end_in_sec > (uint64_t)(512u - sec_off)) end_in_sec = 512u - sec_off;
            for (uint64_t i = 0; i < end_in_sec; i++) {
                uint8_t b = sector[sec_off + i];
                used += popcnt_nibble[b & 0x0Fu] + popcnt_nibble[b >> 4];
            }
            byte_pos += end_in_sec;
        }
        if (rem_bits > 0 && byte_pos == full_bytes) {
            uint32_t sec_off = 0;
            uint64_t slba = bitmap_sector_lba(fs, full_bytes, &sec_off);
            if (slba != UINT64_MAX && block_read(fs->dev, slba, 1, sector) == 0) {
                uint8_t b = sector[sec_off];
                for (uint32_t bit = 0; bit < rem_bits; bit++) {
                    if (b & (uint8_t)(1u << bit)) used++;
                }
            }
        }
        fs->free_clusters = (used <= fs->cluster_count) ? (fs->cluster_count - used) : 0;
        fs->free_known = 1;
    }

    if (free_kb) {
        *free_kb = fs->free_known
                       ? (((uint64_t)fs->free_clusters * (uint64_t)fs->cluster_size) / 1024ULL)
                       : 0ULL;
    }
    return 0;
}
