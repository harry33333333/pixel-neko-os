#ifndef EXFAT_H
#define EXFAT_H

#include "blkdev.h"

#define EXFAT_MAX_VOLUMES 8

typedef struct {
    blkdev_t* dev;
    uint64_t partition_lba;
    uint64_t volume_sectors;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t cluster_heap_offset;
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint32_t sector_size;
    uint32_t cluster_size;
    uint32_t sectors_per_cluster;
    uint32_t allocation_bitmap_cluster;
    uint64_t allocation_bitmap_length;
    uint32_t upcase_cluster;
    uint64_t upcase_length;
    uint32_t upcase_checksum;
    uint32_t volume_serial;
    uint16_t revision;
    char volume_label[16];
    uint8_t sector_shift;
    uint8_t cluster_shift;
    int mounted;
} exfat_fs_t;

typedef struct {
    char name[256];
    uint64_t size;
    uint64_t valid_size;
    uint64_t first_cluster;
    uint16_t attributes;
    bool is_dir;
    bool no_fat_chain;
} exfat_file_info_t;

int exfat_mount(blkdev_t* dev, uint64_t partition_lba, exfat_fs_t* fs);
int exfat_list_dir(exfat_fs_t* fs, const char* path,
                   void (*callback)(const exfat_file_info_t*));
int exfat_find(exfat_fs_t* fs, const char* path, exfat_file_info_t* info);
int exfat_read_file(exfat_fs_t* fs, const exfat_file_info_t* info,
                    uint64_t offset, void* buffer, uint32_t size);
int exfat_write_file(exfat_fs_t* fs, const char* path,
                     const void* buffer, uint64_t size);

#endif
