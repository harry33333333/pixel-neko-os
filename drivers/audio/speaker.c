#include "speaker.h"
#include "port.h"
#include "timer.h"
#include <stdint.h>

static void delay_ms(uint32_t ms) {
    if (ms > 0) {
        sleep_ms((int)ms);
    }
}

static void speaker_silence(unsigned int rest_ms) {
    outb(0x61, inb(0x61) & 0xFC);
    if (rest_ms > 0) {
        delay_ms(rest_ms);
    }
}

void speaker_init(void) {
    /* Ensure PIT/TSC calibration runs before PIT Ch2 is used for tones */
    sleep_ms(0);
    // Ensure speaker gate and data are turned off
    outb(0x61, inb(0x61) & 0xFC);
}

void speaker_play(unsigned int freq, unsigned int duration_ms) {
    if (freq < 20 || freq > 20000 || duration_ms == 0) return;

    /* Ensure TSC calibration is warm so sleep_ms does not reprogram Ch2 mid-note */
    sleep_ms(0);

    unsigned int div = 1193182U / freq;
    if (div == 0) div = 1;
    outb(0x43, 0xB6); // Channel 2, lobyte/hibyte, Mode 3, binary
    outb(0x42, (unsigned char)(div & 0xFF));
    outb(0x42, (unsigned char)((div >> 8) & 0xFF));

    unsigned char tmp = inb(0x61);
    if ((tmp & 3) != 3) {
        outb(0x61, tmp | 3); // enable speaker gate + timer 2 output
    }

    delay_ms(duration_ms);

    // Turn speaker off
    outb(0x61, inb(0x61) & 0xFC);
}

void speaker_boot_chord(void) {
    speaker_play(523, 75);  // C5
    speaker_silence(20);
    speaker_play(659, 75);  // E5
    speaker_silence(20);
    speaker_play(784, 75);  // G5
    speaker_silence(20);
    speaker_play(1046, 150);// C6
    speaker_silence(20);
}

void speaker_beep(void) {
    speaker_play(880, 60);  // A5 standard beep
}

void speaker_shutdown_chord(void) {
    /* Full descending PC-speaker shutdown sequence with crisp articulation & settle pause */
    speaker_play(1046, 110); // C6
    speaker_silence(35);
    speaker_play(784, 110);  // G5
    speaker_silence(35);
    speaker_play(659, 110);  // E5
    speaker_silence(35);
    speaker_play(523, 240);  // C5
    speaker_silence(90);     // Allow VM/hardware audio backend to drain before power-off
}

void speaker_reboot_chord(void) {
    speaker_play(659, 75);  // E5
    speaker_silence(25);
    speaker_play(784, 75);  // G5
    speaker_silence(25);
    speaker_play(1046, 95); // C6
    speaker_silence(25);
    speaker_play(1318, 160);// E6
    speaker_silence(60);
}
