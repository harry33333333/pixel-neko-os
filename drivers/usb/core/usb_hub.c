#include "usb_hub.h"
#include "apic.h"
#include "log.h"
#include "string.h"

typedef struct {
    usb_device_t* device;
    uint8_t ports;
    uint16_t power_good_ms;
} hub_state_t;

static hub_state_t g_hubs[USB_HUB_MAX_DEVICES];
static int g_hub_count = 0;
static uint64_t g_last_poll_tick = 0;

static int hub_request(usb_device_t* hub, int direction, uint8_t request,
                       uint16_t value, uint16_t index, void* data, int length) {
    return usb_control_msg(
        hub, direction | USB_RT_CLASS | USB_RT_OTHER,
        request, value, index, data, length);
}

static int hub_get_port_status(usb_device_t* hub, uint8_t port,
                               uint16_t* status, uint16_t* change) {
    uint32_t buffer = 0;
    int result = hub_request(hub, USB_DIR_IN, USB_REQ_GET_STATUS,
                             0, port, &buffer, 4);
    if (result < 4) return -1;
    if (status) *status = (uint16_t)buffer;
    if (change) *change = (uint16_t)(buffer >> 16);
    return 0;
}

static void hub_clear_changes(usb_device_t* hub, uint8_t port,
                              uint16_t changes) {
    if (changes & HUB_C_PORT_CONNECTION) {
        hub_request(hub, USB_DIR_OUT, USB_REQ_CLEAR_FEATURE,
                    HUB_FEATURE_C_CONNECTION, port, NULL, 0);
    }
    if (changes & HUB_C_PORT_ENABLE) {
        hub_request(hub, USB_DIR_OUT, USB_REQ_CLEAR_FEATURE,
                    HUB_FEATURE_C_ENABLE, port, NULL, 0);
    }
    if (changes & HUB_C_PORT_RESET) {
        hub_request(hub, USB_DIR_OUT, USB_REQ_CLEAR_FEATURE,
                    HUB_FEATURE_C_RESET, port, NULL, 0);
    }
}

static uint8_t hub_port_speed(usb_device_t* hub, uint16_t status) {
    if (status & HUB_PORT_LOW_SPEED) return USB_SPEED_LOW;
    if (status & HUB_PORT_HIGH_SPEED) return USB_SPEED_HIGH;
    if (hub->speed == USB_SPEED_SUPER) return USB_SPEED_SUPER;
    return USB_SPEED_FULL;
}

static void hub_scan_port(usb_device_t* hub, hub_state_t* state, uint8_t port) {
    uint16_t status = 0;
    uint16_t change = 0;
    if (hub_get_port_status(hub, port, &status, &change) != 0) return;

    bool connected = (status & HUB_PORT_CONNECTION) != 0;
    usb_device_t* child = usb_find_child(hub, port);
    if (!connected) {
        if (child) {
            usb_disconnect_child(hub, port);
        }
        hub_clear_changes(hub, port, change);
        return;
    }

    if (child) {
        hub_clear_changes(hub, port, change);
        return;
    }

    log_printf("[HUB ] Device connected: hub=%u port=%u status=0x%04x\n",
               hub->address, port, status);
    for (int debounce = 0; debounce < 10; debounce++) {
        usb_delay_ms(10);
        uint16_t next_status = 0;
        if (hub_get_port_status(hub, port, &next_status, NULL) != 0 ||
            !(next_status & HUB_PORT_CONNECTION)) {
            return;
        }
    }

    hub_request(hub, USB_DIR_OUT, USB_REQ_SET_FEATURE,
                HUB_FEATURE_PORT_RESET, port, NULL, 0);
    usb_delay_ms(20);
    for (int timeout = 0; timeout < 100; timeout++) {
        if (hub_get_port_status(hub, port, &status, &change) != 0) return;
        if (!(status & HUB_PORT_RESET)) break;
        usb_delay_ms(1);
    }
    usb_delay_ms(10);
    hub_clear_changes(hub, port, change);
    if (!(status & HUB_PORT_CONNECTION)) return;

    uint8_t speed = hub_port_speed(hub, status);
    log_printf("[HUB ] Port enabled: hub=%u port=%u speed=%s status=0x%04x\n",
               hub->address, port, usb_speed_name(speed), status);
    if (usb_enumerate_hub(hub->hci, hub, port, speed) == 0) {
        child = usb_find_child(hub, port);
        if (child) {
            hub->child_address[port] = child->address;
            hub->hub_ports_seen |= (uint16_t)(1u << port);
        }
    }
    (void)state;
}

int usb_hub_attach(usb_device_t* hub) {
    if (!hub || g_hub_count >= USB_HUB_MAX_DEVICES) return -1;

    uint8_t descriptor[16];
    memset(descriptor, 0, sizeof(descriptor));
    int result = usb_control_msg(
        hub, USB_DIR_IN | USB_RT_CLASS | USB_RT_DEVICE,
        USB_REQ_GET_DESCRIPTOR, USB_DT_HUB << 8, 0,
        descriptor, sizeof(descriptor));
    if (result < 7 || descriptor[1] != USB_DT_HUB) {
        log_printf("[HUB ] Invalid hub descriptor (result=%d)\n", result);
        return -1;
    }

    uint8_t ports = descriptor[2];
    if (ports == 0 || ports > USB_MAX_HUB_PORTS) {
        log_printf("[HUB ] Invalid hub port count %u\n", ports);
        return -1;
    }
    uint16_t power_good_ms = (uint16_t)descriptor[5] * 2u;

    hub_state_t* state = &g_hubs[g_hub_count++];
    state->device = hub;
    state->ports = ports;
    state->power_good_ms = power_good_ms;
    hub->hub_port_count = ports;

    for (uint8_t port = 1; port <= ports; port++) {
        hub_request(hub, USB_DIR_OUT, USB_REQ_SET_FEATURE,
                    HUB_FEATURE_PORT_POWER, port, NULL, 0);
    }
    uint32_t delay = power_good_ms < 100 ? 100 : power_good_ms;
    usb_delay_ms(delay);
    log_printf("[HUB ] Attached address=%u ports=%u power_good=%ums\n",
               hub->address, ports, power_good_ms);

    for (uint8_t port = 1; port <= ports; port++) {
        hub_scan_port(hub, state, port);
    }
    return 0;
}

void usb_hub_detach(usb_device_t* hub) {
    for (int i = 0; i < g_hub_count; i++) {
        if (g_hubs[i].device != hub) continue;
        g_hubs[i] = g_hubs[g_hub_count - 1];
        g_hub_count--;
        return;
    }
}

void usb_hub_poll(void) {
    uint64_t now = apic_get_ticks();
    if (now - g_last_poll_tick < 500) return;
    g_last_poll_tick = now;

    for (int i = 0; i < g_hub_count; i++) {
        hub_state_t* state = &g_hubs[i];
        if (!state->device || !state->device->hci) continue;
        for (uint8_t port = 1; port <= state->ports; port++) {
            hub_scan_port(state->device, state, port);
        }
    }
}

int usb_hub_count(void) {
    return g_hub_count;
}
