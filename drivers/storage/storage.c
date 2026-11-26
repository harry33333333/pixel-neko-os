#include "storage.h"
#include "ahci.h"
#include "ata.h"
#include "part.h"
#include "log.h"
#include "pci.h"

extern void boot_screen_tick(void);

static bool storage_has_supported_partition(void) {
    for (int index = 0; index < g_partition_count; index++) {
        if (g_partitions[index].is_supported) return true;
    }
    return false;
}

void storage_init(void) {
    /*
     * The P55/H55 PCH can expose the SATA controller as 01:06 (AHCI), as
     * 01:01 (Intel IDE compatibility), or expose both class entries.  Probe
     * AHCI first, but do not treat "a block device was registered" as
     * success: a controller may accept IDENTIFY and still fail every data
     * transfer.  In that case the legacy PIO path is the compatibility
     * fallback that makes the P7H55-M LE boot media readable.
     */
    ahci_init();
    boot_screen_tick();

    if (pci_find_class(0x01, 0x01) != NULL) {
        ata_init();
        boot_screen_tick();
        part_scan_all();
        boot_screen_tick();
    } else {
        part_scan_all();
        boot_screen_tick();
        if (!storage_has_supported_partition()) {
            ata_init();
            boot_screen_tick();
            part_scan_all();
            boot_screen_tick();
        }
    }
    log_printf("[STOR] Block devices ready: %d, partitions: %d\n",
               blkdev_count(), g_partition_count);
}

void storage_rescan(void) {
    part_scan_all();
    log_printf("[STOR] Rescan complete: block devices=%d partitions=%d\n",
               blkdev_count(), g_partition_count);
}

int storage_self_test(void) {
    return blkdev_count() > 0 ? 0 : -1;
}
