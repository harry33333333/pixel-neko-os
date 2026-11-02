#include "usb_hid.h"
#include "input.h"
#include "log.h"
#include "string.h"

typedef enum {
    HID_NONE = 0,
    HID_KEYBOARD,
    HID_MOUSE
} hid_kind_t;

typedef struct {
    bool used;
    usb_device_t* device;
    usb_endpoint_t* endpoint;
    hid_kind_t kind;
    uint8_t report_length;
    uint8_t previous[16];
    uint8_t buttons;
    bool has_wheel;
    uint32_t reports;
} hid_state_t;

static hid_state_t g_hids[USB_HID_MAX_DEVICES];
static int g_keyboards = 0;
static int g_mice = 0;

static usb_endpoint_t* find_interrupt_in(usb_device_t* device) {
    for (uint8_t i = 0; i < device->endpoint_count; i++) {
        usb_endpoint_t* endpoint = &device->endpoints[i];
        if (endpoint->direction == USB_DIR_IN &&
            endpoint->type == USB_EP_TYPE_INTERRUPT) {
            return endpoint;
        }
    }
    return NULL;
}

static bool report_descriptor_has_wheel(const uint8_t* report, int length) {
    uint32_t usage_page = 0;
    int offset = 0;
    while (offset < length) {
        uint8_t prefix = report[offset++];
        if (prefix == 0xFE) {
            if (offset + 2 > length) break;
            offset += 2 + report[offset];
            continue;
        }
        int size = prefix & 3;
        if (size == 3) size = 4;
        if (offset + size > length) break;
        uint32_t value = 0;
        for (int i = 0; i < size; i++) value |= (uint32_t)report[offset + i] << (8 * i);
        uint8_t type = (prefix >> 2) & 3;
        uint8_t tag = (prefix >> 4) & 0x0F;
        if (type == 1 && tag == 0) usage_page = value;
        if (type == 2 && tag == 0 && usage_page == 1 && value == 0x38) {
            return true;
        }
        offset += size;
    }
    return false;
}

static uint32_t hid_usage_to_ascii(uint8_t usage, bool shift, bool caps) {
    if (usage >= 0x04 && usage <= 0x1D) {
        char value = (char)('a' + usage - 0x04);
        return (shift ^ caps) ? (uint32_t)(value - 32) : (uint32_t)value;
    }
    if (usage >= 0x1E && usage <= 0x27) {
        static const char digits[] = "1234567890";
        static const char shifted[] = "!@#$%^&*()";
        return shift ? (uint8_t)shifted[usage - 0x1E]
                     : (uint8_t)digits[usage - 0x1E];
    }
    switch (usage) {
        case 0x28: return INPUT_CODE_ENTER;
        case 0x29: return INPUT_CODE_ESCAPE;
        case 0x2A: return INPUT_CODE_BACKSPACE;
        case 0x2B: return INPUT_CODE_TAB;
        case 0x2C: return ' ';
        case 0x2D: return shift ? '_' : '-';
        case 0x2E: return shift ? '+' : '=';
        case 0x2F: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        case 0x4F: return INPUT_CODE_RIGHT;
        case 0x50: return INPUT_CODE_LEFT;
        case 0x51: return INPUT_CODE_DOWN;
        case 0x52: return INPUT_CODE_UP;
        case 0x4A: return INPUT_CODE_HOME;
        case 0x4D: return INPUT_CODE_END;
        case 0x4B: return INPUT_CODE_PAGE_UP;
        case 0x4E: return INPUT_CODE_PAGE_DOWN;
        case 0x4C: return INPUT_CODE_DELETE;
        default: return 0;
    }
}

static bool report_contains(const uint8_t* report, uint8_t usage) {
    for (int i = 2; i < 8; i++) {
        if (report[i] == usage) return true;
    }
    return false;
}

static void process_keyboard(hid_state_t* hid, const uint8_t* report) {
    bool shift = (report[0] & 0x22U) != 0;
    bool caps = false;

    for (int i = 2; i < 8; i++) {
        uint8_t usage = hid->previous[i];
        if (usage && !report_contains(report, usage)) {
            uint32_t code = hid_usage_to_ascii(usage, shift, caps);
            if (code) input_emit_key(code, false);
        }
    }

    for (int i = 2; i < 8; i++) {
        uint8_t usage = report[i];
        if (!usage || report_contains(hid->previous, usage)) continue;
        if (usage == 0x39) caps = !caps;
        uint32_t code = hid_usage_to_ascii(usage, shift, caps);
        if (code) {
            log_printf("[USBHID] KEY_DOWN code=0x%x '%c'\n",
                       code, (code >= 0x20 && code < 0x7F) ? (char)code : ' ');
            input_emit_key(code, true);
        }
    }
    memcpy(hid->previous, report, 8);
}

static void process_mouse(hid_state_t* hid, const uint8_t* report, int length) {
    int offset = 0;
    if (length > 4) offset = 1; /* Report ID followed by boot mouse fields. */
    if (length >= offset + 3) {
        uint8_t buttons = report[offset] & 0x07U;
        int dx = (int8_t)report[offset + 1];
        int dy = -(int8_t)report[offset + 2];
        int wheel = (length >= offset + 4) ? -(int8_t)report[offset + 3] : 0;
        if (dx || dy) {
            log_printf("[USBHID] MOUSE dx=%d dy=%d buttons=%u\n",
                       dx, dy, buttons);
        }
        input_emit_mouse_move(dx, dy, buttons, wheel);
        if (buttons != hid->buttons) {
            log_printf("[USBHID] BUTTON buttons=%u\n", buttons);
            input_emit_mouse_button(buttons);
            hid->buttons = buttons;
        }
    }
}

static int hid_send_class_request(usb_device_t* device, uint8_t request,
                                  uint16_t value, void* data, int length) {
    uint8_t type = (uint8_t)(device->interface_number ? 1 : 1);
    int direction = length > 0 ? USB_DIR_IN : USB_DIR_OUT;
    return usb_control_msg(
        device, direction | USB_RT_CLASS | type, request, value,
        device->interface_number, data, length);
}

int usb_hid_attach(usb_device_t* device) {
    if (!device || !device->hci) return -1;
    usb_endpoint_t* endpoint = find_interrupt_in(device);
    if (!endpoint) {
        log_printf("[USBHID] No interrupt IN endpoint on device %u\n",
                   device->address);
        return -1;
    }

    hid_kind_t kind = HID_NONE;
    if (device->interface_protocol == USB_HID_PROTOCOL_KBD) kind = HID_KEYBOARD;
    else if (device->interface_protocol == USB_HID_PROTOCOL_MOUSE) kind = HID_MOUSE;
    if (kind == HID_NONE) return -1;

    int slot = -1;
    for (int i = 0; i < USB_HID_MAX_DEVICES; i++) {
        if (!g_hids[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;

    hid_state_t* hid = &g_hids[slot];
    memset(hid, 0, sizeof(*hid));
    hid->used = true;
    hid->device = device;
    hid->endpoint = endpoint;
    hid->kind = kind;
    hid->report_length = endpoint->max_packet;
    if (hid->report_length < 3) hid->report_length = 3;
    if (hid->report_length > sizeof(hid->previous)) {
        hid->report_length = sizeof(hid->previous);
    }

    hid_send_class_request(device, 0x0A, 0, NULL, 0); /* SET_IDLE */
    if (kind == HID_KEYBOARD) {
        hid_send_class_request(device, 0x0B, 0, NULL, 0); /* Boot protocol */
        g_keyboards++;
        log_printf("[USBHID] Attached keyboard address=%u endpoint=0x%02x report=%u\n",
                   device->address, endpoint->address, hid->report_length);
    } else {
        uint8_t report_descriptor[256];
        memset(report_descriptor, 0, sizeof(report_descriptor));
        int report_length = usb_control_msg(
            device, USB_DIR_IN | USB_RT_STANDARD | USB_RT_INTERFACE,
            USB_REQ_GET_DESCRIPTOR, (USB_DT_REPORT << 8), device->interface_number,
            report_descriptor, sizeof(report_descriptor));
        if (report_length > 0) {
            hid->has_wheel = report_descriptor_has_wheel(
                report_descriptor, report_length);
        }
        hid_send_class_request(device, 0x0B, 0, NULL, 0); /* Boot mouse */
        g_mice++;
        log_printf("[USBHID] Attached mouse address=%u endpoint=0x%02x report=%u wheel=%d\n",
                   device->address, endpoint->address, hid->report_length,
                   hid->has_wheel);
    }
    return 0;
}

void usb_hid_detach(usb_device_t* device) {
    for (int i = 0; i < USB_HID_MAX_DEVICES; i++) {
        if (g_hids[i].used && g_hids[i].device == device) {
            if (g_hids[i].kind == HID_KEYBOARD && g_keyboards) g_keyboards--;
            if (g_hids[i].kind == HID_MOUSE && g_mice) g_mice--;
            memset(&g_hids[i], 0, sizeof(g_hids[i]));
            return;
        }
    }
}

void usb_hid_poll(void) {
    for (int i = 0; i < USB_HID_MAX_DEVICES; i++) {
        hid_state_t* hid = &g_hids[i];
        if (!hid->used || !hid->device || !hid->device->hci ||
            !hid->device->hci->intr_in) {
            continue;
        }
        uint8_t report[16];
        memset(report, 0, sizeof(report));
        int length = hid->device->hci->intr_in(
            hid->device->hci, hid->device, hid->endpoint,
            report, hid->report_length);
        if (length <= 0) continue;
        hid->reports++;
        if (hid->reports <= 4) {
            log_printf("[USBHID] report address=%u len=%d data=%02x %02x %02x %02x\n",
                       hid->device->address, length, report[0], report[1],
                       report[2], report[3]);
        }
        if (hid->kind == HID_KEYBOARD && length >= 8) {
            process_keyboard(hid, report);
        } else if (hid->kind == HID_MOUSE) {
            process_mouse(hid, report, length);
        }
    }
}

int usb_hid_keyboard_count(void) {
    return g_keyboards;
}

int usb_hid_mouse_count(void) {
    return g_mice;
}

const char* usb_hid_controller_summary(void) {
    if (g_keyboards || g_mice) return "HID active";
    return "No HID";
}
