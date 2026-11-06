#include "isadma.h"
#include "log.h"
#include "mm.h"
#include "port.h"

#define ISADMA_CHANNEL2_ADDR   0x04
#define ISADMA_CHANNEL2_COUNT  0x05
#define ISADMA_CHANNEL2_PAGE   0x81
#define ISADMA_MODE            0x0B
#define ISADMA_MASK            0x0A
#define ISADMA_CLEAR_FF        0x0C

#define ISADMA_MODE_READ       0x46
#define ISADMA_MODE_WRITE      0x4A
#define ISADMA_CHANNEL2_MASK   0x06

static bool isadma_program(const void* buffer, unsigned int length,
                           unsigned char mode) {
    if (!buffer || length == 0 || length > 0x10000u) return false;

    phys_addr_t physical = 0;
    if (!virt_to_phys_checked(buffer, &physical)) return false;
    if (physical + length - 1 > 0xFFFFFFu) return false;
    if (((physical & 0xFFFFu) + length) > 0x10000u) return false;

    __sync_synchronize();
    outb(ISADMA_MASK, ISADMA_CHANNEL2_MASK);
    outb(ISADMA_CLEAR_FF, 0xFF);
    outb(ISADMA_MODE, mode);
    outb(ISADMA_CHANNEL2_ADDR, (unsigned char)physical);
    outb(ISADMA_CHANNEL2_ADDR, (unsigned char)(physical >> 8));
    outb(ISADMA_CHANNEL2_PAGE, (unsigned char)(physical >> 16));

    unsigned int count = length - 1;
    outb(ISADMA_CHANNEL2_COUNT, (unsigned char)count);
    outb(ISADMA_CHANNEL2_COUNT, (unsigned char)(count >> 8));
    outb(ISADMA_MASK, (unsigned char)(ISADMA_CHANNEL2_MASK & 0x02u));

    log_printf("[DMA ] channel 2 %s: phys=0x%llx bytes=%u\n",
               mode == ISADMA_MODE_READ ? "read" : "write",
               (unsigned long long)physical, length);
    return true;
}

bool isadma_setup_read(void* buffer, unsigned int length) {
    return isadma_program(buffer, length, ISADMA_MODE_READ);
}

bool isadma_setup_write(const void* buffer, unsigned int length) {
    return isadma_program(buffer, length, ISADMA_MODE_WRITE);
}

void isadma_disable(void) {
    outb(ISADMA_MASK, ISADMA_CHANNEL2_MASK);
}
