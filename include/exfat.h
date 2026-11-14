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
    uint32_t alloc_hint;
    uint32_t free_clusters;
    int free_known;
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

typedef struct {
    exfat_fs_t* fs;
    uint64_t size;
    uint64_t valid_size;
    uint64_t first_cluster;
    uint32_t current_cluster;
    uint32_t last_cluster;
    uint64_t current_offset;
    uint32_t cluster_offset;
    uint32_t dir_cluster;
    bool dir_no_fat_chain;
    uint32_t entry_slot;
    bool no_fat_chain;
    bool is_open;
    bool write_mode;
} exfat_handle_t;

int exfat_mount(blkdev_t* dev, uint64_t partition_lba, exfat_fs_t* fs);
int exfat_list_dir(exfat_fs_t* fs, const char* path,
                   void (*callback)(const exfat_file_info_t*));
int exfat_find(exfat_fs_t* fs, const char* path, exfat_file_info_t* info);
exfat_handle_t* exfat_open(exfat_fs_t* fs, const char* path);
exfat_handle_t* exfat_open_write(exfat_fs_t* fs, const char* path);
int exfat_read(exfat_handle_t* handle, void* buffer, uint32_t size);
int exfat_write(exfat_handle_t* handle, const void* buffer, uint32_t size);
int exfat_seek(exfat_handle_t* handle, uint64_t offset);
void exfat_close(exfat_handle_t* handle);
int exfat_read_file(exfat_fs_t* fs, const exfat_file_info_t* info,
                    uint64_t offset, void* buffer, uint32_t size);
int exfat_write_file(exfat_fs_t* fs, const char* path,
                     const void* buffer, uint64_t size);
int exfat_mkdir(exfat_fs_t* fs, const char* path);
int exfat_unlink(exfat_fs_t* fs, const char* path);
int exfat_rename(exfat_fs_t* fs, const char* old_path, const char* new_path);
int exfat_get_volume_space(exfat_fs_t* fs, uint64_t* total_kb, uint64_t* free_kb);

#endif
