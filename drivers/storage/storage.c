#include "storage.h"
#include "ahci.h"
#include "ata.h"
#include "pci.h"
#include "part.h"
#include "log.h"

extern void boot_screen_tick(void);

void storage_init(void) {
    ahci_init();
    boot_screen_tick();
    if (pci_find_class(0x01, 0x01) || blkdev_count() == 0) {
        ata_init();
        boot_screen_tick();
    }
    part_scan_all();
    boot_screen_tick();
    log_printf("[STOR] Block devices ready: %d\n", blkdev_count());
}

int storage_self_test(void) {
    return blkdev_count() > 0 ? 0 : -1;
}
