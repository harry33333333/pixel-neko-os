#ifndef ISO9660_H
#define ISO9660_H

#include "blkdev.h"

typedef struct {
    char           name[64];
    unsigned char  is_dir;
    uint64_t       size;
    uint64_t       extent_lba;
} iso_file_info_t;

typedef struct {
    blkdev_t*      dev;
    uint64_t       root_extent_lba;
    uint64_t       root_data_len;
    uint64_t       total_sectors;
    int            is_joliet;
    char           volume_label[33];
    int            mounted;
} iso_fs_t;

int  iso_mount(blkdev_t* dev, iso_fs_t* fs);
int  iso_list_dir(iso_fs_t* fs, const char* path, void (*callback)(const iso_file_info_t*));
int  iso_find_file(iso_fs_t* fs, const char* path, iso_file_info_t* info);
int  iso_read_file(iso_fs_t* fs, const iso_file_info_t* info, uint64_t offset, unsigned char* buf, unsigned int size);

#endif
