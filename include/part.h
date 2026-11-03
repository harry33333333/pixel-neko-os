#ifndef PART_H
#define PART_H

#include "blkdev.h"

typedef enum {
    PART_FS_UNKNOWN = 0,
    PART_FS_FAT12,
    PART_FS_FAT16,
    PART_FS_FAT32,
    PART_FS_NTFS,
    PART_FS_EXFAT,
    PART_FS_EXT
} part_fs_type_t;

typedef struct {
    blkdev_t*      dev;
    uint64_t       start_lba;
    uint64_t       total_sectors;
    unsigned char  sys_id;          // Partition type byte (e.g. 0x07, 0x0B, 0x83)
    part_fs_type_t fs_type;
    int            is_supported;
    int            is_efi_system;
    uint8_t        gpt_type_guid[16];
    char           label[32];
} partition_entry_t;

#define MAX_PARTITIONS 64
extern partition_entry_t g_partitions[MAX_PARTITIONS];
extern int g_partition_count;

void part_scan_all(void);
partition_entry_t* part_get(int index);
partition_entry_t* part_find_by_start(blkdev_t* dev, uint64_t start_lba);

blkdev_t* part_get_uninit_disk(void);
void part_clear_uninit_disk(void);
void part_initialize_disk(blkdev_t* dev);

#endif
