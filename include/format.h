#ifndef FORMAT_H
#define FORMAT_H

#include "blkdev.h"
#include "part.h"

int format_partition(block_device_t* dev, uint64_t start_lba,
                     uint64_t sector_count, part_fs_type_t type);

#endif
