#include "apic.h"
#include "timer.h"
#include "usb_core.h"

volatile int global_tick;
static int g_timer_irq_mode;

void timer_init(void) {
    global_tick = 0;
    g_timer_irq_mode = 0;
}

void timer_update(void) {
    global_tick = (int)apic_get_ticks();
}

int timer_get_ticks(void) {
    timer_update();
    return global_tick;
}

void sleep_ms(int milliseconds) {
    usb_delay_ms(milliseconds > 0 ? (uint32_t)milliseconds : 0);
    timer_update();
}

void timer_handle_irq(void) {
    global_tick++;
}

void timer_set_irq_mode(int enabled) {
    g_timer_irq_mode = enabled;
}

int timer_irq_mode_enabled(void) {
    return g_timer_irq_mode;
}
