#ifndef BLKDEV_H
#define BLKDEV_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_BLKDEV 32

typedef enum {
    BLK_TYPE_NONE = 0,
    BLK_TYPE_FLOPPY,
    BLK_TYPE_ATA,
    BLK_TYPE_ATAPI,
    BLK_TYPE_AHCI,
    BLK_TYPE_USB,
    BLK_TYPE_MEMORY
} blk_type_t;

typedef enum {
    MEDIA_UNKNOWN = 0,
    MEDIA_PRESENT,
    MEDIA_ABSENT,
    MEDIA_ERROR
} media_state_t;

typedef struct blkdev {
    int id;
    blk_type_t type;
    int unit;
    bool is_removable;
    bool is_readonly;
    uint32_t sector_size;
    uint64_t total_sectors;
    media_state_t media_state;
    uint32_t media_generation;
    char name[32];
    char model[48];
    void* priv;

    int (*read_sectors)(struct blkdev* dev, uint64_t lba, uint32_t count,
                        void* buffer);
    int (*write_sectors)(struct blkdev* dev, uint64_t lba, uint32_t count,
                         const void* buffer);
    media_state_t (*check_media)(struct blkdev* dev);
    int (*eject)(struct blkdev* dev);
} blkdev_t;

typedef blkdev_t block_device_t;

extern blkdev_t g_blkdevs[MAX_BLKDEV];

void blkdev_init(void);
blkdev_t* blkdev_get(int id);
int blkdev_count(void);

blkdev_t* block_register(const char* name, blk_type_t type, int unit,
                         uint32_t sector_size, uint64_t total_sectors,
                         bool is_readonly, bool is_removable,
                         int (*read_blocks)(blkdev_t*, uint64_t, uint32_t,
                                            void*),
                         int (*write_blocks)(blkdev_t*, uint64_t, uint32_t,
                                             const void*),
                         media_state_t (*check_media)(blkdev_t*),
                         void* priv);
void block_unregister(blkdev_t* dev);
int block_read(blkdev_t* dev, uint64_t lba, uint32_t count, void* buffer);
int block_write(blkdev_t* dev, uint64_t lba, uint32_t count,
                const void* buffer);
media_state_t block_check_media(blkdev_t* dev);
void block_set_capacity(blkdev_t* dev, uint64_t sectors,
                        uint32_t sector_size);
blkdev_t* blkdev_find_type(blk_type_t type, int unit);

#endif
