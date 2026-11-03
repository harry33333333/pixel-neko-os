#include "power.h"
#include "log.h"
#include "port.h"

void sys_reboot(void) {
    log_printf("[POWER] Rebooting through the i8042 reset line\n");
    for (int timeout = 0; timeout < 100000; timeout++) {
        if (!(inb(0x64) & 0x02)) break;
    }
    outb(0x64, 0xFE);
    for (;;) __asm__ volatile ("cli; hlt");
}

void sys_shutdown(void) {
    log_printf("[POWER] Safe shutdown fallback active\n");
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}
