#include "iso9660.h"
#include "log.h"
#include <stddef.h>

/*
 * ISO9660 extent numbers are always in 2048-byte logical sectors.  A USB
 * installation image is normally exposed as a 512-byte block device, while
 * ATAPI media reports 2048-byte sectors.  Translate the ISO logical address
 * instead of calling the block device directly with a 2048-byte buffer.
 */
static int iso_read_sector(blkdev_t* dev, uint64_t iso_lba,
                           unsigned char* buffer) {
    if (!dev || !buffer || !dev->read_sectors || !dev->sector_size) return -1;
    if (dev->sector_size == 2048) {
        return block_read(dev, iso_lba, 1, buffer);
    }
    if (dev->sector_size < 2048 && 2048u % dev->sector_size == 0) {
        uint32_t count = 2048u / dev->sector_size;
        uint64_t lba = iso_lba * count;
        return block_read(dev, lba, count, buffer);
    }
    if (dev->sector_size > 2048 && dev->sector_size <= 4096 &&
        dev->sector_size % 2048u == 0) {
        unsigned char physical[4096];
        uint32_t sectors_per_iso = dev->sector_size / 2048u;
        uint64_t lba = iso_lba / sectors_per_iso;
        uint32_t offset = (uint32_t)(iso_lba % sectors_per_iso) * 2048u;
        if (block_read(dev, lba, 1, physical) != 0) return -1;
        for (uint32_t index = 0; index < 2048u; index++) {
            buffer[index] = physical[offset + index];
        }
        return 0;
    }
    return -1;
}

static int str_case_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        i++;
    }
    return (a[i] == 0 && b[i] == 0);
}

int iso_mount(blkdev_t* dev, iso_fs_t* fs) {
    if (!dev || !fs || !dev->read_sectors) return -1;
    fs->dev = dev;
    fs->mounted = 0;
    fs->is_joliet = 0;
    fs->volume_label[0] = '\0';

    unsigned char sec[2048];
    int found_pvd = 0;

    for (unsigned int lba = 16; lba < 32; lba++) {
        if (iso_read_sector(dev, lba, sec) != 0) {
            log_printf("[ISO ] Read failed at sector %u\n", lba);
            break;
        }
        if (sec[1] != 'C' || sec[2] != 'D' || sec[3] != '0' || sec[4] != '0' || sec[5] != '1') {
            log_printf("[ISO ] Sector %u header=%02x %02x %02x %02x %02x %02x\n",
                       lba, sec[0], sec[1], sec[2], sec[3], sec[4], sec[5]);
            break;
        }

        unsigned char type = sec[0];
        if (type == 255) break; // Terminator

        // Check for Joliet Supplementary Volume Descriptor (Type 2)
        if (type == 2) {
            if (sec[88] == 0x25 && sec[89] == 0x2F &&
                (sec[90] == 0x40 || sec[90] == 0x43 || sec[90] == 0x45)) {
                fs->is_joliet = 1;
                unsigned char* root_rec = &sec[156];
                fs->root_extent_lba = *(unsigned int*)&root_rec[2];
                fs->root_data_len = *(unsigned int*)&root_rec[10];
                fs->total_sectors = *(unsigned int*)&sec[80];

                // Joliet Volume Label (UCS-2 BE)
                int lp = 0;
                for (int i = 40; i < 72 && lp < 31; i += 2) {
                    char c = sec[i + 1];
                    if (c == 0 || c == ' ') break;
                    fs->volume_label[lp++] = c;
                }
                fs->volume_label[lp] = '\0';
                fs->mounted = 1;
                return 0; // Joliet is preferred
            }
        } else if (type == 1 && !found_pvd) {
            // Standard Primary Volume Descriptor (Type 1)
            found_pvd = 1;
            unsigned char* root_rec = &sec[156];
            fs->root_extent_lba = *(unsigned int*)&root_rec[2];
            fs->root_data_len = *(unsigned int*)&root_rec[10];
            fs->total_sectors = *(unsigned int*)&sec[80];

            int lp = 0;
            for (int i = 40; i < 72 && lp < 31; i++) {
                char c = sec[i];
                if (c == ' ' && lp > 0 && sec[i + 1] == ' ') break;
                if (c != 0) fs->volume_label[lp++] = c;
            }
            fs->volume_label[lp] = '\0';
            fs->mounted = 1;
        }
    }

    return fs->mounted ? 0 : -1;
}

typedef int (*iso_dirent_visitor_t)(const iso_file_info_t* item, void* user_data);

static int parse_directory(iso_fs_t* fs, unsigned int start_lba, unsigned int total_len, iso_dirent_visitor_t visitor, void* user_data) {
    unsigned char sec[2048];
    if (total_len == 0) return 0;
    unsigned int sectors = (total_len + 2047) / 2048;
    if (sectors > 1024u) sectors = 1024u;

    for (unsigned int s = 0; s < sectors; s++) {
        if (iso_read_sector(fs->dev, start_lba + s, sec) != 0) {
            return -1;
        }

        unsigned int offset = 0;
        while (offset < 2048) {
            unsigned char rec_len = sec[offset];
            if (rec_len == 0) {
                // No more records in this sector; move to next sector
                break;
            }
            if (rec_len < 33 || offset + rec_len > 2048) {
                break;
            }

            unsigned char* rec = &sec[offset];
            unsigned char len_fi = rec[32];
            unsigned char flags = rec[25];

            // Ignore current directory (0) and parent directory (1) special entries
            if (len_fi == 1 && (rec[33] == 0 || rec[33] == 1)) {
                offset += rec_len;
                continue;
            }

            iso_file_info_t item;
            item.extent_lba = *(unsigned int*)&rec[2];
            item.size = *(unsigned int*)&rec[10];
            item.is_dir = (flags & 0x02) ? 1 : 0;

            int name_len = 0;
            if (fs->is_joliet) {
                // Unicode UCS-2 Big Endian -> ASCII
                for (int i = 0; i < len_fi && name_len < 63; i += 2) {
                    char c = rec[33 + i + 1];
                    if (c == 0) c = rec[33 + i];
                    if (c == 0) break;
                    if (c == ';') break; // Strip version suffix ;1
                    item.name[name_len++] = c;
                }
                if (name_len > 0 && item.name[name_len - 1] == '.') {
                    name_len--;
                }
            } else {
                for (int i = 0; i < len_fi && name_len < 63; i++) {
                    char c = rec[33 + i];
                    if (c == ';') break; // Strip version suffix ;1
                    item.name[name_len++] = c;
                }
                // Strip trailing dot if present
                if (name_len > 0 && item.name[name_len - 1] == '.') {
                    name_len--;
                }
            }
            item.name[name_len] = '\0';

            if (visitor(&item, user_data) != 0) {
                return 1; // Visited item matched / abort early
            }

            offset += rec_len;
        }
    }
    return 0;
}

struct list_ctx {
    void (*cb)(const iso_file_info_t*);
};

static int list_visitor(const iso_file_info_t* item, void* ud) {
    struct list_ctx* ctx = (struct list_ctx*)ud;
    if (ctx->cb) ctx->cb(item);
    return 0;
}

struct find_ctx {
    const char* target_name;
    iso_file_info_t* out_info;
    int found;
};

static int find_visitor(const iso_file_info_t* item, void* ud) {
    struct find_ctx* ctx = (struct find_ctx*)ud;
    if (str_case_eq(item->name, ctx->target_name)) {
        *ctx->out_info = *item;
        ctx->found = 1;
        return 1;
    }
    return 0;
}

static const char* get_next_path_comp(const char* path, char* comp, int max_comp) {
    while (*path == '\\' || *path == '/') path++;
    int i = 0;
    while (*path && *path != '\\' && *path != '/') {
        if (i < max_comp - 1) comp[i++] = *path;
        path++;
    }
    comp[i] = '\0';
    return path;
}

int iso_find_file(iso_fs_t* fs, const char* path, iso_file_info_t* info) {
    if (!fs || !fs->mounted) return -1;

    // Skip drive letter prefix if present (e.g. "E:\" or "C:\")
    if (path[0] && path[1] == ':') {
        path += 2;
    }
    while (*path == '\\' || *path == '/') path++;

    if (*path == '\0') {
        info->extent_lba = fs->root_extent_lba;
        info->size = fs->root_data_len;
        info->is_dir = 1;
        info->name[0] = '\0';
        return 0;
    }

    unsigned int cur_lba = fs->root_extent_lba;
    unsigned int cur_len = fs->root_data_len;
    char comp[64];

    while (*path) {
        path = get_next_path_comp(path, comp, sizeof(comp));
        if (comp[0] == '\0') break;

        struct find_ctx ctx = { comp, info, 0 };
        if (parse_directory(fs, cur_lba, cur_len, find_visitor, &ctx) != 1 || !ctx.found) {
            return -1; // Component not found
        }

        if (*path) {
            if (!info->is_dir) return -1;
            cur_lba = info->extent_lba;
            cur_len = info->size;
        }
    }

    return 0;
}

int iso_list_dir(iso_fs_t* fs, const char* path, void (*callback)(const iso_file_info_t*)) {
    if (!fs || !fs->mounted) return -1;
    iso_file_info_t dir_info;
    if (iso_find_file(fs, path, &dir_info) != 0) return -1;
    if (!dir_info.is_dir) return -1;

    struct list_ctx ctx = { callback };
    parse_directory(fs, dir_info.extent_lba, dir_info.size, list_visitor, &ctx);
    return 0;
}

static int iso_read_sectors_multi(blkdev_t* dev, uint64_t iso_lba,
                                  uint32_t iso_count, unsigned char* buffer) {
    if (!dev || !buffer || !dev->read_sectors || !dev->sector_size || !iso_count) {
        return -1;
    }
    if (dev->is_removable && dev->media_state != MEDIA_PRESENT) {
        return -1;
    }
    if (dev->sector_size == 2048) {
        return block_read(dev, iso_lba, iso_count, buffer);
    }
    if (dev->sector_size < 2048 && (2048u % dev->sector_size) == 0) {
        uint32_t factor = 2048u / dev->sector_size;
        return block_read(dev, iso_lba * factor, iso_count * factor, buffer);
    }
    for (uint32_t i = 0; i < iso_count; i++) {
        if (iso_read_sector(dev, iso_lba + i, buffer + (size_t)i * 2048u) != 0) {
            return -1;
        }
    }
    return 0;
}

int iso_read_extent(iso_fs_t* fs, uint64_t extent_lba, uint64_t file_size,
                    uint64_t offset, unsigned char* buf, unsigned int size,
                    unsigned int* cached_lba, int* has_cached,
                    unsigned char* sec_cache) {
    if (!fs || !fs->mounted || !fs->dev || !buf) return -1;
    if (fs->dev->is_removable && fs->dev->media_state != MEDIA_PRESENT) {
        return -1;
    }
    if (offset >= file_size) return 0;
    if (size > file_size - offset) {
        size = (unsigned int)(file_size - offset);
    }
    if (size == 0) return 0;

    unsigned int bytes_read = 0;
    unsigned char local_sec[2048];
    unsigned char* work_sec = sec_cache ? sec_cache : local_sec;

    while (bytes_read < size) {
        uint64_t cur_offset = offset + bytes_read;
        unsigned int sec_idx = (unsigned int)(cur_offset / 2048u);
        unsigned int sec_off = (unsigned int)(cur_offset % 2048u);
        uint64_t target_lba = extent_lba + sec_idx;
        unsigned int rem = size - bytes_read;

        /* Batch contiguous full-sector reads directly into caller buffer */
        if (sec_off == 0 && rem >= 2048u) {
            uint32_t sectors_want = rem / 2048u;
            if (sectors_want > 16u) sectors_want = 16u;
            if (iso_read_sectors_multi(fs->dev, target_lba, sectors_want,
                                       buf + bytes_read) != 0) {
                return bytes_read > 0 ? (int)bytes_read : -1;
            }
            if (sec_cache && cached_lba && has_cached) {
                const unsigned char* last =
                    buf + bytes_read + (size_t)(sectors_want - 1u) * 2048u;
                for (unsigned int k = 0; k < 2048u; k++) sec_cache[k] = last[k];
                *cached_lba = (unsigned int)(target_lba + sectors_want - 1u);
                *has_cached = 1;
            }
            bytes_read += sectors_want * 2048u;
            continue;
        }

        bool cache_hit = (sec_cache && has_cached && *has_cached &&
                          cached_lba && target_lba < 0xFFFFFFFFULL &&
                          *cached_lba == (unsigned int)target_lba);
        if (!cache_hit) {
            if (has_cached) *has_cached = 0;
            if (iso_read_sector(fs->dev, target_lba, work_sec) != 0) {
                return bytes_read > 0 ? (int)bytes_read : -1;
            }
            if (sec_cache && cached_lba && has_cached) {
                *cached_lba = (unsigned int)target_lba;
                *has_cached = 1;
            }
        }

        unsigned int to_copy = 2048u - sec_off;
        if (to_copy > rem) to_copy = rem;
        for (unsigned int i = 0; i < to_copy; i++) {
            buf[bytes_read + i] = work_sec[sec_off + i];
        }
        bytes_read += to_copy;
    }

    return (int)bytes_read;
}

int iso_read_file(iso_fs_t* fs, const iso_file_info_t* info, uint64_t offset,
                  unsigned char* buf, unsigned int size) {
    if (!fs || !fs->mounted || !info || info->is_dir) return -1;
    return iso_read_extent(fs, info->extent_lba, info->size, offset, buf, size,
                           NULL, NULL, NULL);
}
