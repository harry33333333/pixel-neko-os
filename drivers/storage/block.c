#include "blkdev.h"
#include "log.h"
#include "string.h"

blkdev_t g_blkdevs[MAX_BLKDEV];
static int g_block_count;

static void block_copy_name(char* target, unsigned int capacity,
                            const char* source) {
    unsigned int index = 0;
    if (!target || capacity == 0) return;
    while (source && source[index] && index + 1 < capacity) {
        target[index] = source[index];
        index++;
    }
    target[index] = '\0';
}

void blkdev_init(void) {
    memset(g_blkdevs, 0, sizeof(g_blkdevs));
    g_block_count = 0;
}

blkdev_t* block_register(const char* name, blk_type_t type, int unit,
                         uint32_t sector_size, uint64_t total_sectors,
                         bool is_readonly, bool is_removable,
                         int (*read_blocks)(blkdev_t*, uint64_t, uint32_t,
                                            void*),
                         int (*write_blocks)(blkdev_t*, uint64_t, uint32_t,
                                             const void*),
                         media_state_t (*check_media)(blkdev_t*),
                         void* priv) {
    if (g_block_count >= MAX_BLKDEV || !read_blocks) return NULL;
    blkdev_t* dev = &g_blkdevs[g_block_count];
    memset(dev, 0, sizeof(*dev));
    dev->id = g_block_count++;
    dev->type = type;
    dev->unit = unit;
    dev->sector_size = sector_size ? sector_size : 512;
    dev->total_sectors = total_sectors;
    dev->is_readonly = is_readonly;
    dev->is_removable = is_removable;
    dev->media_state = total_sectors ? MEDIA_PRESENT : MEDIA_ABSENT;
    dev->media_generation = 1;
    dev->read_sectors = read_blocks;
    dev->write_sectors = write_blocks;
    dev->check_media = check_media;
    dev->priv = priv;
    block_copy_name(dev->name, sizeof(dev->name), name);
    block_copy_name(dev->model, sizeof(dev->model), name);
    log_printf("[BLK ] Registered %s: %llu x %u-byte sectors%s\n",
               dev->name, (unsigned long long)dev->total_sectors,
               dev->sector_size, dev->is_readonly ? " (RO)" : "");
    return dev;
}

void block_unregister(blkdev_t* dev) {
    if (!dev || dev < g_blkdevs || dev >= g_blkdevs + g_block_count) {
        return;
    }
    int index = (int)(dev - g_blkdevs);
    if (index != g_block_count - 1) {
        g_blkdevs[index] = g_blkdevs[g_block_count - 1];
        g_blkdevs[index].id = index;
    }
    memset(&g_blkdevs[g_block_count - 1], 0, sizeof(g_blkdevs[0]));
    g_block_count--;
}

blkdev_t* blkdev_get(int id) {
    return id >= 0 && id < g_block_count ? &g_blkdevs[id] : NULL;
}

int blkdev_count(void) {
    return g_block_count;
}

blkdev_t* blkdev_find_type(blk_type_t type, int unit) {
    for (int i = 0; i < g_block_count; i++) {
        if (g_blkdevs[i].type == type &&
            (unit < 0 || g_blkdevs[i].unit == unit)) {
            return &g_blkdevs[i];
        }
    }
    return NULL;
}

int block_read(blkdev_t* dev, uint64_t lba, uint32_t count, void* buffer) {
    if (!dev || !dev->read_sectors || !buffer || !count) return -1;
    if (dev->is_removable && dev->media_state != MEDIA_PRESENT) return -1;
    if (lba >= dev->total_sectors || count > dev->total_sectors - lba) {
        return -1;
    }
    return dev->read_sectors(dev, lba, count, buffer);
}

int block_write(blkdev_t* dev, uint64_t lba, uint32_t count,
                const void* buffer) {
    if (!dev || !dev->write_sectors || !buffer || !count) return -1;
    if (dev->is_readonly || lba >= dev->total_sectors ||
        count > dev->total_sectors - lba) {
        return -1;
    }
    return dev->write_sectors(dev, lba, count, buffer);
}

media_state_t block_check_media(blkdev_t* dev) {
    if (!dev) return MEDIA_ERROR;
    media_state_t state = dev->check_media
                              ? dev->check_media(dev)
                              : dev->media_state;
    if (state != dev->media_state) {
        dev->media_state = state;
        dev->media_generation++;
    }
    return state;
}

void block_set_capacity(blkdev_t* dev, uint64_t sectors,
                        uint32_t sector_size) {
    if (!dev) return;
    dev->total_sectors = sectors;
    if (sector_size) dev->sector_size = sector_size;
    dev->media_state = sectors ? MEDIA_PRESENT : MEDIA_ABSENT;
    dev->media_generation++;
}
