#include "usb_hid.h"
#include "input.h"
#include "log.h"
#include "string.h"
#include "wm.h"

#define HID_MAX_REPORT_IDS 8
#define HID_MAX_LOCAL_USAGES 16
#define HID_MAX_KEYS 16
#define HID_REPORT_MAX_BYTES 64

typedef struct {
    bool used;
    uint8_t report_id;
    uint16_t bit_end;
    uint16_t modifiers_offset;
    uint8_t modifiers_count;
    uint16_t keys_offset;
    uint8_t keys_size;
    uint8_t keys_count;
    uint16_t buttons_offset;
    uint8_t buttons_count;
    uint16_t x_offset;
    uint16_t y_offset;
    uint16_t wheel_offset;
    uint8_t x_size;
    uint8_t y_size;
    uint8_t wheel_size;
    int32_t x_min;
    int32_t x_max;
    int32_t y_min;
    int32_t y_max;
    bool x_absolute;
    bool y_absolute;
} hid_report_layout_t;

typedef struct {
    bool keyboard;
    bool mouse;
    uint8_t report_count;
    uint8_t report_length;
    hid_report_layout_t reports[HID_MAX_REPORT_IDS];
} hid_descriptor_t;

typedef struct {
    bool used;
    usb_device_t* device;
    usb_endpoint_t* endpoint;
    uint8_t interface_number;
    bool keyboard;
    bool mouse;
    bool boot_keyboard;
    uint8_t report_length;
    uint32_t buttons;
    bool caps_lock;
    uint8_t previous_keys[HID_MAX_KEYS];
    uint32_t previous_key_codes[HID_MAX_KEYS];
    int32_t last_x;
    int32_t last_y;
    bool have_absolute_position;
    uint32_t reports;
    hid_descriptor_t descriptor;
} hid_state_t;

typedef struct {
    uint32_t usage_page;
    uint32_t report_size;
    uint32_t report_count;
    uint8_t report_id;
    int32_t logical_min;
    int32_t logical_max;
} hid_global_state_t;

typedef struct {
    uint32_t usage_page;
    uint32_t report_size;
    uint32_t report_count;
    uint8_t report_id;
    int32_t logical_min;
    int32_t logical_max;
    uint32_t usages[HID_MAX_LOCAL_USAGES];
    uint8_t usage_count;
    uint32_t usage_min;
    uint32_t usage_max;
    bool has_usage_min;
    bool has_usage_max;
} hid_item_state_t;

static hid_state_t g_hids[USB_HID_MAX_DEVICES];
static int g_keyboards = 0;
static int g_mice = 0;

static uint32_t hid_item_value(const uint8_t* data, int size) {
    uint32_t value = 0;
    for (int i = 0; i < size; i++) {
        value |= (uint32_t)data[i] << (8 * i);
    }
    return value;
}

static int32_t hid_item_signed_value(const uint8_t* data, int size) {
    if (size <= 0) return 0;
    uint32_t value = hid_item_value(data, size);
    if (size < 4 && (value & (1u << (size * 8 - 1)))) {
        value |= UINT32_MAX << (size * 8);
    }
    return (int32_t)value;
}

static uint32_t hid_full_usage(uint32_t page, uint32_t usage, int size) {
    if (size == 4 && (usage >> 16)) return usage;
    return (page << 16) | (usage & 0xFFFFu);
}

static hid_report_layout_t* report_layout_for_id(hid_descriptor_t* descriptor,
                                                 uint8_t report_id) {
    for (uint8_t i = 0; i < descriptor->report_count; i++) {
        if (descriptor->reports[i].used &&
            descriptor->reports[i].report_id == report_id) {
            return &descriptor->reports[i];
        }
    }
    if (descriptor->report_count >= HID_MAX_REPORT_IDS) return NULL;
    hid_report_layout_t* layout =
        &descriptor->reports[descriptor->report_count++];
    memset(layout, 0, sizeof(*layout));
    layout->used = true;
    layout->report_id = report_id;
    return layout;
}

static void record_input_field(hid_descriptor_t* descriptor,
                               hid_report_layout_t* layout,
                               const hid_item_state_t* state,
                               bool absolute) {
    uint32_t count = state->report_count;
    uint32_t size = state->report_size;
    if (!count || !size) return;
    if (count > 512u) count = 512u;
    if (size > 32u) size = 32u;

    uint16_t field_start = layout->bit_end;
    for (uint32_t field = 0; field < count; field++) {
        uint32_t field_offset = (uint32_t)field_start + field * size;
        if (field_offset > UINT16_MAX) return;
        uint32_t usage = 0;
        if (field < state->usage_count) {
            usage = state->usages[field];
        } else if (state->has_usage_min && state->has_usage_max) {
            uint32_t raw_min = state->usage_min;
            uint32_t raw_max = state->usage_max;
            if (raw_min + field <= raw_max) {
                usage = hid_full_usage(state->usage_page,
                                       (raw_min & 0xFFFFu) + field, 2);
            }
        } else if (state->usage_count) {
            usage = state->usages[state->usage_count - 1];
        }

        uint32_t page = usage >> 16;
        uint32_t code = usage & 0xFFFFu;
        if (page == 0x0001u) {
            if (code == 0x01u || code == 0x02u) descriptor->mouse = true;
            if (code == 0x06u || code == 0x07u) descriptor->keyboard = true;
            if (code == 0x30u && !layout->x_size) {
                descriptor->mouse = true;
                layout->x_offset = (uint16_t)field_offset;
                layout->x_size = (uint8_t)(size > 32 ? 32 : size);
                layout->x_min = state->logical_min;
                layout->x_max = state->logical_max;
                layout->x_absolute = absolute;
            } else if (code == 0x31u && !layout->y_size) {
                descriptor->mouse = true;
                layout->y_offset = (uint16_t)field_offset;
                layout->y_size = (uint8_t)(size > 32 ? 32 : size);
                layout->y_min = state->logical_min;
                layout->y_max = state->logical_max;
                layout->y_absolute = absolute;
            } else if (code == 0x38u && !layout->wheel_size) {
                descriptor->mouse = true;
                layout->wheel_offset = (uint16_t)field_offset;
                layout->wheel_size = (uint8_t)(size > 32 ? 32 : size);
            }
        } else if (page == 0x0007u) {
            descriptor->keyboard = true;
            if (size == 1 && code >= 0xE0u && code <= 0xE7u) {
                if (!layout->modifiers_count) {
                    layout->modifiers_offset = field_start;
                    layout->modifiers_count =
                        (uint8_t)(count > 8 ? 8 : count);
                }
            } else if (size >= 8 && !layout->keys_count) {
                layout->keys_offset = field_start;
                layout->keys_size = (uint8_t)(size > 16 ? 16 : size);
                layout->keys_count =
                    (uint8_t)(count > HID_MAX_KEYS ? HID_MAX_KEYS : count);
            }
        } else if (page == 0x0009u) {
            descriptor->mouse = true;
            if (!layout->buttons_count) {
                layout->buttons_offset = field_start;
                layout->buttons_count =
                    (uint8_t)(count > 32 ? 32 : count);
            }
        }

        if (layout->bit_end > UINT16_MAX - size) {
            layout->bit_end = UINT16_MAX;
            return;
        }
        layout->bit_end = (uint16_t)(layout->bit_end + size);
    }
}

static void parse_report_descriptor(const uint8_t* report, int length,
                                    hid_descriptor_t* descriptor) {
    hid_item_state_t state;
    hid_global_state_t stack[4];
    int stack_depth = 0;
    memset(&state, 0, sizeof(state));
    memset(stack, 0, sizeof(stack));

    int offset = 0;
    while (offset < length) {
        uint8_t prefix = report[offset++];
        if (prefix == 0xFEu) {
            if (offset + 2 > length) break;
            int data_size = report[offset];
            offset += 2 + data_size;
            continue;
        }
        int size = prefix & 3;
        if (size == 3) size = 4;
        if (offset + size > length) break;

        uint8_t type = (uint8_t)((prefix >> 2) & 3);
        uint8_t tag = (uint8_t)((prefix >> 4) & 0x0F);
        uint32_t value = hid_item_value(report + offset, size);
        int32_t signed_value = hid_item_signed_value(report + offset, size);
        offset += size;

        if (type == 0) {
            /* Main item: Input (0x8), Output (0x9), Feature (0xB), Collection (0xA/0xC) */
            if (tag == 0x8) {
                hid_report_layout_t* layout = report_layout_for_id(
                    descriptor, state.report_id);
                if (layout) {
                    if (value & 1u) {
                        /* HIO_CONST (padding/constant field) */
                        uint64_t bits =
                            (uint64_t)state.report_count * state.report_size;
                        uint64_t end = (uint64_t)layout->bit_end + bits;
                        layout->bit_end =
                            end > UINT16_MAX ? UINT16_MAX : (uint16_t)end;
                    } else {
                        /*
                         * FreeBSD sys/dev/usb/usb_hid.h:
                         * Bit 2 (0x04, HIO_RELATIVE): 0 = Absolute, 1 = Relative.
                         */
                        bool absolute = (value & 4u) == 0;
                        record_input_field(descriptor, layout, &state, absolute);
                    }
                }
            }
            state.usage_count = 0;
            state.has_usage_min = false;
            state.has_usage_max = false;
        } else if (type == 1) {
            /* Global item */
            switch (tag) {
                case 0x0: state.usage_page = value; break;
                case 0x1: state.logical_min = signed_value; break;
                case 0x2:
                    if (state.logical_min >= 0 && signed_value < state.logical_min &&
                        size <= 2) {
                        state.logical_max = (int32_t)value;
                    } else {
                        state.logical_max = signed_value;
                    }
                    break;
                case 0x7: state.report_size = value; break;
                case 0x8: state.report_id = (uint8_t)value; break;
                case 0x9: state.report_count = value; break;
                case 0xA: /* Push */
                    if (stack_depth < 4) {
                        stack[stack_depth].usage_page = state.usage_page;
                        stack[stack_depth].report_size = state.report_size;
                        stack[stack_depth].report_count = state.report_count;
                        stack[stack_depth].report_id = state.report_id;
                        stack[stack_depth].logical_min = state.logical_min;
                        stack[stack_depth].logical_max = state.logical_max;
                        stack_depth++;
                    }
                    break;
                case 0xB: /* Pop */
                    if (stack_depth > 0) {
                        stack_depth--;
                        state.usage_page = stack[stack_depth].usage_page;
                        state.report_size = stack[stack_depth].report_size;
                        state.report_count = stack[stack_depth].report_count;
                        state.report_id = stack[stack_depth].report_id;
                        state.logical_min = stack[stack_depth].logical_min;
                        state.logical_max = stack[stack_depth].logical_max;
                    }
                    break;
            }
        } else if (type == 2) {
            /* Local item */
            if (tag == 0x0) {
                if (state.usage_count < HID_MAX_LOCAL_USAGES) {
                    state.usages[state.usage_count++] =
                        hid_full_usage(state.usage_page, value, size);
                }
                uint32_t usage = hid_full_usage(state.usage_page, value, size);
                if ((usage >> 16) == 0x0001u) {
                    uint32_t ucode = usage & 0xFFFFu;
                    if (ucode == 0x01u || ucode == 0x02u) {
                        descriptor->mouse = true;
                    } else if (ucode == 0x06u || ucode == 0x07u) {
                        descriptor->keyboard = true;
                    }
                }
            } else if (tag == 0x1) {
                state.usage_min = hid_full_usage(
                    state.usage_page, value, size);
                state.has_usage_min = true;
            } else if (tag == 0x2) {
                state.usage_max = hid_full_usage(
                    state.usage_page, value, size);
                state.has_usage_max = true;
            }
        }
    }

    uint8_t maximum_length = 0;
    for (uint8_t i = 0; i < descriptor->report_count; i++) {
        hid_report_layout_t* layout = &descriptor->reports[i];
        if (!layout->used) continue;
        uint32_t bytes = (layout->bit_end + 7u) / 8u;
        if (layout->report_id) bytes++;
        if (bytes > HID_REPORT_MAX_BYTES) bytes = HID_REPORT_MAX_BYTES;
        if (bytes > maximum_length) maximum_length = (uint8_t)bytes;
    }
    descriptor->report_length = maximum_length;
}

static bool report_descriptor_has_wheel(const uint8_t* report, int length) {
    hid_descriptor_t descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    parse_report_descriptor(report, length, &descriptor);
    for (uint8_t i = 0; i < descriptor.report_count; i++) {
        if (descriptor.reports[i].wheel_size) return true;
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

static uint32_t read_report_bits(const uint8_t* report, int length,
                                 uint16_t bit_offset, uint8_t size) {
    if (!report || size == 0 || size > 32 ||
        (uint32_t)bit_offset + size > (uint32_t)length * 8u) {
        return 0;
    }
    uint32_t value = 0;
    for (uint8_t bit = 0; bit < size; bit++) {
        uint32_t source = (uint32_t)bit_offset + bit;
        if (report[source / 8u] & (1u << (source & 7u))) {
            value |= 1u << bit;
        }
    }
    return value;
}

static int32_t read_report_signed(const uint8_t* report, int length,
                                  uint16_t bit_offset, uint8_t size) {
    uint32_t value = read_report_bits(report, length, bit_offset, size);
    if (size > 0 && size < 32 && (value & (1u << (size - 1)))) {
        value |= UINT32_MAX << size;
    }
    return (int32_t)value;
}

static bool key_usage_present(const uint8_t* keys, int count,
                              uint8_t usage) {
    for (int i = 0; i < count; i++) {
        if (keys[i] == usage) return true;
    }
    return false;
}

static hid_report_layout_t* layout_for_report(hid_state_t* hid,
                                              const uint8_t* report,
                                              int length, int* payload_offset) {
    if (!report || length <= 0) return NULL;
    for (uint8_t i = 0; i < hid->descriptor.report_count; i++) {
        hid_report_layout_t* layout = &hid->descriptor.reports[i];
        if (layout->used && layout->report_id &&
            report[0] == layout->report_id) {
            *payload_offset = 1;
            return layout;
        }
    }
    for (uint8_t i = 0; i < hid->descriptor.report_count; i++) {
        hid_report_layout_t* layout = &hid->descriptor.reports[i];
        if (layout->used && layout->report_id == 0) {
            *payload_offset = 0;
            return layout;
        }
    }
    return NULL;
}

static void process_keyboard(hid_state_t* hid, hid_report_layout_t* layout,
                             const uint8_t* report, int length) {
    uint32_t modifiers = 0;
    if (layout->modifiers_count) {
        modifiers = read_report_bits(
            report, length, layout->modifiers_offset,
            layout->modifiers_count > 32 ? 32 : layout->modifiers_count);
    }
    bool shift = (modifiers & 0x22u) != 0;
    uint8_t current_keys[HID_MAX_KEYS];
    uint32_t current_codes[HID_MAX_KEYS];
    memset(current_keys, 0, sizeof(current_keys));
    memset(current_codes, 0, sizeof(current_codes));
    int key_count = 0;

    for (uint8_t i = 0; i < layout->keys_count && key_count < HID_MAX_KEYS;
         i++) {
        uint32_t usage = read_report_bits(
            report, length,
            (uint16_t)(layout->keys_offset + (uint16_t)i * layout->keys_size),
            layout->keys_size);
        if (usage > 0xFFu) usage &= 0xFFu;
        if (usage) current_keys[key_count++] = (uint8_t)usage;
    }

    int previous_count = 0;
    while (previous_count < HID_MAX_KEYS &&
           hid->previous_keys[previous_count]) {
        previous_count++;
    }
    for (int i = 0; i < previous_count; i++) {
        uint8_t usage = hid->previous_keys[i];
        if (!key_usage_present(current_keys, key_count, usage) &&
            hid->previous_key_codes[i]) {
            input_emit_key(hid->previous_key_codes[i], false);
        }
    }

    for (int i = 0; i < key_count; i++) {
        uint8_t usage = current_keys[i];
        if (key_usage_present(hid->previous_keys, previous_count, usage)) {
            continue;
        }
        if (usage == 0x39) hid->caps_lock = !hid->caps_lock;
        uint32_t code = hid_usage_to_ascii(usage, shift, hid->caps_lock);
        current_codes[i] = code;
        if (code) {
            log_printf("[USBHID] KEY_DOWN code=0x%x '%c'\n",
                       code, (code >= 0x20 && code < 0x7F) ? (char)code : ' ');
            input_emit_key(code, true);
        }
    }

    memset(hid->previous_keys, 0, sizeof(hid->previous_keys));
    memset(hid->previous_key_codes, 0, sizeof(hid->previous_key_codes));
    memcpy(hid->previous_keys, current_keys, (size_t)key_count);
    memcpy(hid->previous_key_codes, current_codes,
           sizeof(uint32_t) * (size_t)key_count);
}

static int32_t clamp_i32(int32_t value, int32_t minimum, int32_t maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static uint32_t g_last_combined_buttons = 0;

static void process_mouse(hid_state_t* hid, hid_report_layout_t* layout,
                          const uint8_t* report, int length) {
    if (layout->buttons_count) {
        hid->buttons = read_report_bits(
            report, length, layout->buttons_offset,
            layout->buttons_count > 32 ? 32 : layout->buttons_count);
    }

    uint32_t buttons = 0;
    for (int i = 0; i < USB_HID_MAX_DEVICES; i++) {
        if (g_hids[i].used && g_hids[i].mouse) {
            buttons |= g_hids[i].buttons;
        }
    }

    int32_t x = 0;
    int32_t y = 0;
    int32_t wheel = 0;
    if (layout->x_size) {
        x = (layout->x_absolute && layout->x_min >= 0)
                ? (int32_t)read_report_bits(report, length, layout->x_offset,
                                            layout->x_size)
                : read_report_signed(report, length, layout->x_offset,
                                     layout->x_size);
    }
    if (layout->y_size) {
        y = (layout->y_absolute && layout->y_min >= 0)
                ? (int32_t)read_report_bits(report, length, layout->y_offset,
                                            layout->y_size)
                : read_report_signed(report, length, layout->y_offset,
                                     layout->y_size);
    }
    if (layout->wheel_size) {
        wheel = -read_report_signed(report, length, layout->wheel_offset,
                                    layout->wheel_size);
    }

    if (layout->x_absolute || layout->y_absolute) {
        int32_t sw = screen_w > 0 ? screen_w : 1024;
        int32_t sh = screen_h > 0 ? screen_h : 768;
        int32_t abs_x = hid->have_absolute_position ? hid->last_x : (sw / 2);
        int32_t abs_y = hid->have_absolute_position ? hid->last_y : (sh / 2);

        if (layout->x_absolute) {
            int32_t xmin = layout->x_min;
            int32_t xmax = layout->x_max > xmin ? layout->x_max : 32767;
            int32_t rx = clamp_i32(x, xmin, xmax);
            abs_x = (int32_t)(((int64_t)(rx - xmin) * (sw - 1)) / (xmax - xmin));
        } else {
            abs_x = clamp_i32(abs_x + x, 0, sw - 1);
        }

        if (layout->y_absolute) {
            int32_t ymin = layout->y_min;
            int32_t ymax = layout->y_max > ymin ? layout->y_max : 32767;
            int32_t ry = clamp_i32(y, ymin, ymax);
            abs_y = (int32_t)(((int64_t)(ry - ymin) * (sh - 1)) / (ymax - ymin));
        } else {
            abs_y = clamp_i32(abs_y + y, 0, sh - 1);
        }

        abs_x = clamp_i32(abs_x, 0, sw - 1);
        abs_y = clamp_i32(abs_y, 0, sh - 1);

        int32_t dx = hid->have_absolute_position ? (abs_x - hid->last_x)
                                                 : (abs_x - sw / 2);
        int32_t dy = hid->have_absolute_position ? (abs_y - hid->last_y)
                                                 : (abs_y - sh / 2);
        if (!hid->have_absolute_position && dx == 0 && dy == 0) {
            dx = 1;
        }
        hid->last_x = abs_x;
        hid->last_y = abs_y;
        hid->have_absolute_position = true;

        if ((dx || dy) && hid->reports <= 8) {
            log_printf("[USBHID] MOUSE dx=%d dy=%d buttons=%u\n",
                       dx, dy, buttons);
        }
        input_emit_mouse_abs(abs_x, abs_y, dx, dy, buttons, wheel);
    } else {
        /*
         * Per USB HID Usage Tables 1.12 Section 4.1 & FreeBSD ums.c:
         * USB HID relative X is positive right, Y is positive down (screen Y).
         */
        x = clamp_i32(x, -32767, 32767);
        y = clamp_i32(y, -32767, 32767);
        if ((x || y) && hid->reports <= 8) {
            log_printf("[USBHID] MOUSE dx=%d dy=%d buttons=%u\n",
                       x, y, buttons);
        }
        input_emit_mouse_move(x, y, buttons, wheel);
    }

    if (buttons != g_last_combined_buttons) {
        if (hid->reports <= 8) {
            log_printf("[USBHID] BUTTON buttons=%u\n", buttons);
        }
        input_emit_mouse_button(buttons);
        g_last_combined_buttons = buttons;
    }
}

static int hid_send_class_request(usb_device_t* device, uint8_t interface_num,
                                  uint8_t request, uint16_t value,
                                  void* data, int length) {
    int direction = length > 0 ? USB_DIR_IN : USB_DIR_OUT;
    return usb_control_msg(
        device, direction | USB_RT_CLASS | USB_RT_INTERFACE, request, value,
        interface_num, data, length);
}

static bool ensure_boot_layout(hid_descriptor_t* descriptor, bool keyboard) {
    hid_report_layout_t* layout = NULL;
    for (uint8_t i = 0; i < descriptor->report_count; i++) {
        hid_report_layout_t* candidate = &descriptor->reports[i];
        if (!candidate->used) continue;
        if (keyboard ? candidate->keys_count
                     : (candidate->buttons_count || candidate->x_size || candidate->y_size)) {
            layout = candidate;
            break;
        }
    }
    if (!layout) layout = report_layout_for_id(descriptor, 0);
    if (!layout) return false;

    if (keyboard) {
        if (!layout->modifiers_count) {
            layout->modifiers_offset = 0;
            layout->modifiers_count = 8;
        }
        if (!layout->keys_count) {
            layout->keys_offset = 16;
            layout->keys_size = 8;
            layout->keys_count = 6;
        }
        if (!descriptor->report_length) descriptor->report_length = 8;
    } else {
        bool has_any_mouse = layout->buttons_count || layout->x_size ||
                             layout->y_size;
        if (!has_any_mouse) {
            layout->buttons_offset = 0;
            layout->buttons_count = 3;
            layout->x_offset = 8;
            layout->x_size = 8;
            layout->y_offset = 16;
            layout->y_size = 8;
            if (!layout->wheel_size && descriptor->report_length > 3) {
                layout->wheel_offset = 24;
                layout->wheel_size = 8;
            }
            if (!descriptor->report_length) descriptor->report_length = 4;
        }
    }
    return true;
}

static int attach_hid_endpoint(usb_device_t* device, usb_endpoint_t* endpoint) {
    uint8_t if_num = endpoint->interface_number;
    uint8_t if_subclass = endpoint->interface_subclass;
    uint8_t if_proto = endpoint->interface_protocol;

    bool boot_capable = (if_subclass == USB_HID_SUBCLASS_BOOT);

    /*
     * FreeBSD ukbd.c / ums.c:
     * Only send SET_IDLE / SET_PROTOCOL on Boot SubClass interfaces,
     * and do so before reading the Report Descriptor.
     */
    if (boot_capable) {
        (void)hid_send_class_request(device, if_num, 0x0A, 0, NULL, 0);
    }

    uint8_t report_descriptor[512];
    memset(report_descriptor, 0, sizeof(report_descriptor));
    uint16_t req_len = endpoint->hid_report_desc_len;
    if (req_len == 0 || req_len > sizeof(report_descriptor)) {
        req_len = 256;
    }
    int descriptor_length = usb_control_msg(
        device, USB_DIR_IN | USB_RT_STANDARD | USB_RT_INTERFACE,
        USB_REQ_GET_DESCRIPTOR, USB_DT_REPORT << 8, if_num,
        report_descriptor, req_len);
    if (descriptor_length <= 0 && req_len != sizeof(report_descriptor)) {
        descriptor_length = usb_control_msg(
            device, USB_DIR_IN | USB_RT_STANDARD | USB_RT_INTERFACE,
            USB_REQ_GET_DESCRIPTOR, USB_DT_REPORT << 8, if_num,
            report_descriptor, sizeof(report_descriptor));
    }

    hid_descriptor_t descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    if (descriptor_length > 0) {
        parse_report_descriptor(report_descriptor, descriptor_length,
                                &descriptor);
    }

    if (!descriptor.keyboard && if_proto == USB_HID_PROTOCOL_KBD) {
        descriptor.keyboard = true;
        ensure_boot_layout(&descriptor, true);
    }
    if (!descriptor.mouse && if_proto == USB_HID_PROTOCOL_MOUSE) {
        descriptor.mouse = true;
        ensure_boot_layout(&descriptor, false);
    }
    if (!descriptor.keyboard && !descriptor.mouse) {
        return -1;
    }

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
    hid->interface_number = if_num;
    hid->keyboard = descriptor.keyboard;
    hid->mouse = descriptor.mouse;
    hid->boot_keyboard = boot_capable && descriptor.keyboard &&
                         !descriptor.mouse;
    hid->descriptor = descriptor;
    if (hid->boot_keyboard) ensure_boot_layout(&hid->descriptor, true);
    if (hid->mouse) ensure_boot_layout(&hid->descriptor, false);

    uint8_t report_length = (uint8_t)(endpoint->max_packet > HID_REPORT_MAX_BYTES
                                          ? HID_REPORT_MAX_BYTES
                                          : endpoint->max_packet);
    if (report_length < hid->descriptor.report_length) {
        report_length = hid->descriptor.report_length;
    }
    if (report_length < 3) report_length = 3;
    if (report_length > HID_REPORT_MAX_BYTES) {
        report_length = HID_REPORT_MAX_BYTES;
    }
    hid->report_length = report_length;

    if (boot_capable) {
        if (hid->boot_keyboard) {
            (void)hid_send_class_request(device, if_num, 0x0B, 0, NULL, 0);
        } else {
            (void)hid_send_class_request(device, if_num, 0x0B, 1, NULL, 0);
        }
    }

    if (hid->keyboard) {
        g_keyboards++;
        log_printf("[USBHID] Attached keyboard address=%u endpoint=0x%02x report=%u mode=%s\n",
                   device->address, endpoint->address, hid->report_length,
                   hid->boot_keyboard ? "boot" : "report");
    }
    if (hid->mouse) {
        g_mice++;
        bool wheel = false;
        for (uint8_t i = 0; i < hid->descriptor.report_count; i++) {
            if (hid->descriptor.reports[i].wheel_size) wheel = true;
        }
        log_printf("[USBHID] Attached mouse address=%u endpoint=0x%02x report=%u wheel=%d mode=report\n",
                   device->address, endpoint->address, hid->report_length,
                   wheel);
    }
    return 0;
}

int usb_hid_attach(usb_device_t* device) {
    if (!device || !device->hci) return -1;

    int attached = 0;
    for (uint8_t i = 0; i < device->endpoint_count; i++) {
        usb_endpoint_t* endpoint = &device->endpoints[i];
        if (endpoint->direction != USB_DIR_IN ||
            endpoint->type != USB_EP_TYPE_INTERRUPT) {
            continue;
        }
        if (endpoint->interface_class != 0 &&
            endpoint->interface_class != USB_CLASS_HID) {
            continue;
        }
        if (attach_hid_endpoint(device, endpoint) == 0) {
            attached++;
        }
    }

    if (attached == 0) {
        log_printf("[USBHID] No attachable HID interrupt IN endpoint on device %u (subclass=%u protocol=%u)\n",
                   device->address, device->interface_subclass,
                   device->interface_protocol);
        return -1;
    }
    return 0;
}

void usb_hid_detach(usb_device_t* device) {
    for (int i = 0; i < USB_HID_MAX_DEVICES; i++) {
        if (g_hids[i].used && g_hids[i].device == device) {
            if (g_hids[i].keyboard && g_keyboards) g_keyboards--;
            if (g_hids[i].mouse && g_mice) g_mice--;
            memset(&g_hids[i], 0, sizeof(g_hids[i]));
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
        for (int pkt = 0; pkt < 4; pkt++) {
            uint8_t report[HID_REPORT_MAX_BYTES];
            memset(report, 0, sizeof(report));
            int length = hid->device->hci->intr_in(
                hid->device->hci, hid->device, hid->endpoint, report,
                hid->report_length);
            if (length <= 0) break;
            hid->reports++;
            if (hid->reports <= 8) {
                log_printf("[USBHID] report address=%u len=%d data=%02x %02x %02x %02x\n",
                           hid->device->address, length, report[0], report[1],
                           report[2], report[3]);
            }

            int payload_offset = 0;
            hid_report_layout_t* layout =
                layout_for_report(hid, report, length, &payload_offset);
            if (!layout) continue;
            const uint8_t* payload = report + payload_offset;
            int payload_length = length - payload_offset;
            if (hid->keyboard && layout->keys_count) {
                process_keyboard(hid, layout, payload, payload_length);
            }
            if (hid->mouse && (layout->buttons_count || layout->x_size ||
                               layout->y_size)) {
                process_mouse(hid, layout, payload, payload_length);
            }
        }
    }
}

int usb_hid_selftest(void) {
    static const uint8_t keyboard_descriptor[] = {
        0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
        0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
        0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
        0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
        0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
        0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0
    };
    static const uint8_t mouse_descriptor[] = {
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
        0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
        0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
        0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F,
        0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
        0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01,
        0x81, 0x06, 0xC0, 0xC0
    };

    hid_descriptor_t keyboard;
    hid_descriptor_t mouse;
    memset(&keyboard, 0, sizeof(keyboard));
    memset(&mouse, 0, sizeof(mouse));
    parse_report_descriptor(keyboard_descriptor,
                            sizeof(keyboard_descriptor), &keyboard);
    parse_report_descriptor(mouse_descriptor, sizeof(mouse_descriptor),
                            &mouse);
    if (!keyboard.keyboard || !keyboard.report_count ||
        !mouse.mouse || !report_descriptor_has_wheel(
                            mouse_descriptor, sizeof(mouse_descriptor))) {
        log_printf("[USBHID] Descriptor parser self-test FAILED\n");
        return -1;
    }
    log_printf("[USBHID] Descriptor parser self-test PASS (generic keyboard + mouse)\n");
    return 0;
}

int usb_hid_keyboard_count(void) {
    return g_keyboards;
}

int usb_hid_mouse_count(void) {
    return g_mice;
}
