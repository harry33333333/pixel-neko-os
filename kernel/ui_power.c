#include "power.h"
#include "acpi.h"
#include "audio.h"
#include "fat.h"
#include "font.h"
#include "log.h"
#include "port.h"
#include "speaker.h"
#include "wm.h"

void sys_reboot(void) {
    log_printf("[POWER] Preparing system reboot...\n");
    audio_stop();
    speaker_reboot_chord();
    fat_flush_all();

    acpi_reboot();

    for (int timeout = 0; timeout < 100000; timeout++) {
        if (!(inb(0x64) & 0x02)) break;
    }
    outb(0x64, 0xFE);
    for (;;) __asm__ volatile ("cli; hlt");
}

void sys_shutdown(void) {
    log_printf("[POWER] Preparing ACPI S5 shutdown...\n");
    audio_stop();
    speaker_shutdown_chord();
    fat_flush_all();

    acpi_shutdown();

    /* If hardware did not power off, display the classic safe power-off screen */
    log_printf("[POWER] Safe shutdown fallback screen active\n");
    for (int y = 0; y < screen_h; y++) {
        for (int x = 0; x < screen_w; x++) {
            PUT_PIXEL(backbuffer, x, y, 0);
        }
    }
    const char* msg = "It is now safe to turn off your computer.";
    int len = 0;
    while (msg[len]) len++;
    int tx = (screen_w - len * 8) / 2;
    int ty = (screen_h - 16) / 2;
    font_draw_text(backbuffer, tx, ty, msg, 14);
    copy_fullscreen_to_lfb(backbuffer);

    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}
