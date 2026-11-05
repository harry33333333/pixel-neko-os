#include "mouse.h"
#include "keyboard.h"
#include "input.h"
#include "device.h"
#include "boot_framebuffer.h"
#include "idt.h"
#include "port.h"
#include "log.h"
#include "string.h"
#include "usb_hid.h"

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_COMMAND_PORT 0x64

static uint8_t g_packet[3];
static uint32_t g_packet_index = 0;
static int g_mouse_x = 0;
static int g_mouse_y = 0;
static uint32_t g_buttons = 0;
static int g_mouse_speed = 2;
static bool g_mouse_ready = false;
static bool g_controller_ready = false;
static mouse_button_event_t g_legacy_events[32];
static volatile uint32_t g_legacy_head = 0;
static volatile uint32_t g_legacy_tail = 0;

static bool ps2_wait_write(void) {
    for (uint32_t i = 0; i < 100000; i++) {
        if ((inb(PS2_STATUS_PORT) & 0x02u) == 0) return true;
    }
    return false;
}

static bool ps2_wait_read(void) {
    for (uint32_t i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS_PORT) & 0x01u) return true;
    }
    return false;
}

static void ps2_drain(void) {
    for (uint32_t i = 0; i < 64; i++) {
        if (!(inb(PS2_STATUS_PORT) & 0x01u)) break;
        (void)inb(PS2_DATA_PORT);
    }
}

static bool ps2_command(uint8_t command) {
    if (!ps2_wait_write()) return false;
    outb(PS2_COMMAND_PORT, command);
    return true;
}

static bool ps2_write_data(uint8_t value) {
    if (!ps2_wait_write()) return false;
    outb(PS2_DATA_PORT, value);
    return true;
}

static bool ps2_read_data(uint8_t* value) {
    if (!value || !ps2_wait_read()) return false;
    *value = inb(PS2_DATA_PORT);
    return true;
}

static bool ps2_mouse_command(uint8_t command) {
    uint8_t response = 0;
    if (!ps2_command(0xD4) || !ps2_write_data(command) ||
        !ps2_read_data(&response)) {
        return false;
    }
    return response == 0xFA;
}

static void legacy_push_button(uint32_t old_buttons, uint32_t new_buttons) {
    uint32_t difference = old_buttons ^ new_buttons;
    if (!difference) return;
    uint32_t next = (g_legacy_head + 1u) % 32u;
    if (next == g_legacy_tail) return;
    mouse_button_event_t* event = &g_legacy_events[g_legacy_head];
    event->x = g_mouse_x;
    event->y = g_mouse_y;
    event->buttons = (int)new_buttons;
    event->pressed = (int)(difference & new_buttons);
    event->released = (int)(difference & old_buttons);
    event->tick = 0;
    g_legacy_head = next;
}

void mouse_init(void) {
    g_mouse_x = (int)g_main_surface.width / 2;
    g_mouse_y = (int)g_main_surface.height / 2;
    g_packet_index = 0;
    g_buttons = 0;
    g_mouse_speed = 2;

    ps2_drain();
    uint8_t config = 0;
    if (!ps2_command(0x20) || !ps2_read_data(&config)) {
        log_printf("[PS2 ] Controller command byte unavailable; preserving firmware mapping.\n");
        g_controller_ready = false;
        return;
    }

    if (config & 0x20u) {
        (void)ps2_command(0xA8);
    }

    uint8_t wanted = (uint8_t)((config | 0x43u) & ~0x30u);
    if (!ps2_command(0x60) || !ps2_write_data(wanted)) {
        log_printf("[PS2 ] Failed to enable IRQ1/IRQ12; using current mapping.\n");
        g_controller_ready = false;
        return;
    }

    if (ps2_mouse_command(0xF6)) {
        if (!ps2_mouse_command(0xF4)) {
            log_printf("[PS2 ] Mouse rejected streaming enable.\n");
        } else {
            g_mouse_ready = true;
        }
    } else {
        log_printf("[PS2 ] Mouse did not acknowledge sample-rate command.\n");
    }

    g_controller_ready = true;
    log_printf("[PS2 ] Controller ready: IRQ1 keyboard, IRQ12 mouse, translation=%s.\n",
               (wanted & 0x40u) ? "on" : "off");
}

void mouse_handle_byte(uint8_t byte) {
    if (g_packet_index == 0 && ((byte & 0x08u) == 0 ||
                                (byte & 0xC0u) != 0)) {
        return;
    }

    g_packet[g_packet_index++] = byte;
    if (g_packet_index < 3) return;
    g_packet_index = 0;

    uint32_t old_buttons = g_buttons;
    g_buttons = g_packet[0] & 0x07u;
    int dx = g_packet[1];
    int dy = g_packet[2];
    if (g_packet[0] & 0x10u) dx -= 256;
    if (g_packet[0] & 0x20u) dy -= 256;

    dx = dx * g_mouse_speed / 2;
    dy = -(dy * g_mouse_speed / 2);
    g_mouse_x += dx;
    g_mouse_y += dy;
    if (g_mouse_x < 0) g_mouse_x = 0;
    if (g_mouse_y < 0) g_mouse_y = 0;
    if (g_mouse_x >= (int)g_main_surface.width) {
        g_mouse_x = (int)g_main_surface.width - 1;
    }
    if (g_mouse_y >= (int)g_main_surface.height) {
        g_mouse_y = (int)g_main_surface.height - 1;
    }

    if (usb_hid_prefer_mouse_input()) return;
    input_emit_mouse_move(dx, dy, g_buttons, 0);
    if (old_buttons != g_buttons) {
        input_emit_mouse_button(g_buttons);
        legacy_push_button(old_buttons, g_buttons);
    }
}

void ps2_irq_handler(void) {
    for (uint32_t budget = 0; budget < 32; budget++) {
        uint8_t status = inb(PS2_STATUS_PORT);
        if (!(status & 0x01u)) break;
        uint8_t data = inb(PS2_DATA_PORT);
        if (status & 0x20u) {
            mouse_handle_byte(data);
        } else {
            keyboard_handle_byte(data);
        }
    }
}

static void irq1_handler(trap_frame_t* frame) {
    (void)frame;
    ps2_irq_handler();
}

static void irq12_handler(trap_frame_t* frame) {
    (void)frame;
    ps2_irq_handler();
}

static bool ps2_driver_match(const device_t* device) {
    return device && strcmp(device->name, "i8042") == 0;
}

static int ps2_driver_probe(device_t* device) {
    (void)device;
    mouse_init();
    if (!g_mouse_ready && !g_controller_ready) {
        log_printf("[PS2 ] Running in conservative firmware-emulation mode.\n");
    }
    irq_install_handler(1, irq1_handler);
    irq_install_handler(12, irq12_handler);
    return 0;
}

static device_driver_t g_ps2_driver = {
    .name = "ps2-i8042",
    .bus_type = BUS_TYPE_ISA,
    .match = ps2_driver_match,
    .probe = ps2_driver_probe
};

void ps2_input_init(void) {
    keyboard_init();
    g_controller_ready = false;
    g_mouse_ready = false;

    bus_t* isa = bus_find(BUS_TYPE_ISA);
    if (!isa) isa = bus_register("isa", BUS_TYPE_ISA);
    device_t* controller = device_register(isa, "i8042", 0x0000, 0x0000,
                                           0x09, 0x00, NULL);
    if (controller) {
        device_add_resource(controller, DEVICE_RESOURCE_IO, 0x60, 1, false);
        device_add_resource(controller, DEVICE_RESOURCE_IO, 0x64, 1, false);
        device_add_resource(controller, DEVICE_RESOURCE_IRQ, 1, 1, true);
        device_add_resource(controller, DEVICE_RESOURCE_IRQ, 12, 1, true);
    }
    device_driver_register(&g_ps2_driver);
}

void mouse_get(int* x, int* y, int* buttons) {
    if (x) *x = g_mouse_x;
    if (y) *y = g_mouse_y;
    if (buttons) *buttons = (int)g_buttons;
}

int mouse_take_button_event(mouse_button_event_t* event) {
    if (!event || g_legacy_head == g_legacy_tail) return 0;
    *event = g_legacy_events[g_legacy_tail];
    g_legacy_tail = (g_legacy_tail + 1u) % 32u;
    return 1;
}

int mouse_get_speed(void) {
    return g_mouse_speed;
}

void mouse_set_speed(int speed) {
    if (speed >= 1 && speed <= 5) g_mouse_speed = speed;
}
