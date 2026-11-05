#ifndef ATA_H
#define ATA_H

#include "blkdev.h"

void ata_init(void);
int ata_atapi_eject(blkdev_t* device);

#endif
