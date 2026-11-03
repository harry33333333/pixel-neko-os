#include "storage.h"
#include "ahci.h"
#include "ata.h"
#include "pci.h"
#include "part.h"
#include "log.h"

void storage_init(void) {
    ahci_init();
    if (pci_find_class(0x01, 0x01) || blkdev_count() == 0) {
        ata_init();
    }
    part_scan_all();
    log_printf("[STOR] Block devices ready: %d\n", blkdev_count());
}

int storage_self_test(void) {
    return blkdev_count() > 0 ? 0 : -1;
}
