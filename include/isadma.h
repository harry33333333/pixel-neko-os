#ifndef ISADMA_H
#define ISADMA_H

#include <stdbool.h>

/*
 * ISA DMA channel 2, used by the primary floppy controller.
 * The buffer must be below 16 MiB and must not cross a 64 KiB boundary.
 * "read" means device-to-memory; "write" means memory-to-device.
 */
bool isadma_setup_read(void* buffer, unsigned int length);
bool isadma_setup_write(const void* buffer, unsigned int length);
void isadma_disable(void);

#endif
