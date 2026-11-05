#ifndef AHCI_H
#define AHCI_H

#include "blkdev.h"

void ahci_init(void);
int ahci_atapi_eject(blkdev_t* device);

#endif
