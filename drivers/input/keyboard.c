#include "keyboard.h"
#include "input.h"

static bool g_ps2_shift = false;
static bool g_ps2_ctrl = false;
static bool g_ps2_alt = false;
static bool g_usb_shift = false;
static bool g_usb_ctrl = false;
static bool g_usb_alt = false;
static bool g_caps = false;
static bool g_extended = false;
static uint8_t g_e1_remaining = 0;
static volatile int g_taskmgr_hotkey = 0;
static volatile int g_ime_toggle_hotkey = 0;
static volatile int g_alt_tab_steps = 0;
static bool g_ime_chord_seen = false;
static bool g_ime_chord_tainted = false;

static int g_key_queue[64];
static volatile uint32_t g_key_head = 0;
static volatile uint32_t g_key_tail = 0;

static void legacy_push(int key) {
    uint32_t next = (g_key_head + 1u) % 64u;
    if (next == g_key_tail) return;
    g_key_queue[g_key_head] = key;
    g_key_head = next;
}

static const uint8_t normal_map[128] = {
    [0x01] = 0x1B, [0x02] = '1', [0x03] = '2', [0x04] = '3',
    [0x05] = '4', [0x06] = '5', [0x07] = '6', [0x08] = '7',
    [0x09] = '8', [0x0A] = '9', [0x0B] = '0', [0x0C] = '-',
    [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n', [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd',
    [0x21] = 'f', [0x22] = 'g', [0x23] = 'h', [0x24] = 'j',
    [0x25] = 'k', [0x26] = 'l', [0x27] = ';', [0x28] = '\'',
    [0x29] = '`', [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x',
    [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b', [0x31] = 'n',
    [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x37] = '*', [0x39] = ' '
};

static const uint8_t shift_map[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
    [0x1A] = '{', [0x1B] = '}', [0x27] = ':', [0x28] = '"',
    [0x29] = '~', [0x2B] = '|', [0x33] = '<', [0x34] = '>',
    [0x35] = '?'
};

static uint32_t translate(uint8_t scancode) {
    bool shift = g_ps2_shift || g_usb_shift;
    uint8_t base = normal_map[scancode & 0x7F];
    if (base >= 'a' && base <= 'z') {
        return (shift ^ g_caps) ? (uint32_t)(base - 32) : base;
    }
    if (shift && shift_map[scancode & 0x7F]) {
        return shift_map[scancode & 0x7F];
    }
    return base;
}

void keyboard_init(void) {
    g_ps2_shift = false;
    g_ps2_ctrl = false;
    g_ps2_alt = false;
    g_usb_shift = false;
    g_usb_ctrl = false;
    g_usb_alt = false;
    g_caps = false;
    g_extended = false;
    g_e1_remaining = 0;
    g_taskmgr_hotkey = 0;
    g_ime_toggle_hotkey = 0;
    g_alt_tab_steps = 0;
    g_ime_chord_seen = false;
    g_ime_chord_tainted = false;
    g_key_head = 0;
    g_key_tail = 0;
}

static void keyboard_update_ime_chord(bool non_mod_key) {
    bool ctrl = g_ps2_ctrl || g_usb_ctrl;
    bool shift = g_ps2_shift || g_usb_shift;
    if (non_mod_key) {
        g_ime_chord_seen = false;
        if (ctrl || shift) g_ime_chord_tainted = true;
        return;
    }
    if (ctrl && shift) {
        if (!g_ime_chord_tainted) g_ime_chord_seen = true;
    } else {
        if (g_ime_chord_seen && !g_ime_chord_tainted) {
            g_ime_toggle_hotkey = 1;
        }
        g_ime_chord_seen = false;
        if (!ctrl && !shift) {
            g_ime_chord_tainted = false;
        }
    }
}

void keyboard_handle_byte(uint8_t scancode) {
    if (scancode == 0xE1) {
        g_e1_remaining = 2;
        return;
    }
    if (g_e1_remaining) {
        g_e1_remaining--;
        return;
    }
    if (scancode == 0xE0) {
        g_extended = true;
        return;
    }

    bool release = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;
    uint32_t translated = 0;

    if (g_extended) {
        switch (code) {
            case 0x1D:
                g_ps2_ctrl = !release;
                keyboard_update_ime_chord(false);
                g_extended = false;
                return;
            case 0x38:
                g_ps2_alt = !release;
                keyboard_update_ime_chord(true);
                g_extended = false;
                return;
            case 0x48: translated = INPUT_CODE_UP; break;
            case 0x50: translated = INPUT_CODE_DOWN; break;
            case 0x4B: translated = INPUT_CODE_LEFT; break;
            case 0x4D: translated = INPUT_CODE_RIGHT; break;
            case 0x47: translated = INPUT_CODE_HOME; break;
            case 0x4F: translated = INPUT_CODE_END; break;
            case 0x49: translated = INPUT_CODE_PAGE_UP; break;
            case 0x51: translated = INPUT_CODE_PAGE_DOWN; break;
            case 0x53: translated = INPUT_CODE_DELETE; break;
            default: break;
        }
        g_extended = false;
    } else {
        switch (code) {
            case 0x2A:
            case 0x36:
                g_ps2_shift = !release;
                keyboard_update_ime_chord(false);
                return;
            case 0x1D:
                g_ps2_ctrl = !release;
                keyboard_update_ime_chord(false);
                return;
            case 0x38:
                g_ps2_alt = !release;
                keyboard_update_ime_chord(true);
                return;
            case 0x3A:
                if (!release) g_caps = !g_caps;
                keyboard_update_ime_chord(true);
                return;
            default:
                translated = translate(code);
                break;
        }
    }

    if (!translated) return;
    if (!release) keyboard_update_ime_chord(true);
    if (translated == INPUT_CODE_TAB && keyboard_get_alt()) {
        if (!release) {
            keyboard_notify_alt_tab(keyboard_get_shift());
        }
        return;
    }
    if (!release && translated == INPUT_CODE_ESCAPE &&
        keyboard_get_ctrl() && keyboard_get_shift()) {
        g_taskmgr_hotkey = 1;
        return;
    }
    input_emit_key(translated, !release);
}

int keyboard_get_key(void) {
    if (g_key_head == g_key_tail) return 0;
    int key = g_key_queue[g_key_tail];
    g_key_tail = (g_key_tail + 1u) % 64u;
    return key;
}

int keyboard_get_ctrl(void) {
    return (g_ps2_ctrl || g_usb_ctrl) ? 1 : 0;
}

int keyboard_get_shift(void) {
    return (g_ps2_shift || g_usb_shift) ? 1 : 0;
}

int keyboard_get_alt(void) {
    return (g_ps2_alt || g_usb_alt) ? 1 : 0;
}

void keyboard_set_usb_modifiers(int ctrl, int shift, int alt) {
    g_usb_ctrl = (ctrl != 0);
    g_usb_shift = (shift != 0);
    g_usb_alt = (alt != 0);
    keyboard_update_ime_chord(false);
}

void keyboard_note_usb_activity(int has_keys) {
    if (has_keys) keyboard_update_ime_chord(true);
}

void keyboard_notify_taskmgr_hotkey(void) {
    g_taskmgr_hotkey = 1;
    keyboard_update_ime_chord(true);
}

int keyboard_consume_taskmgr_hotkey(void) {
    int v = g_taskmgr_hotkey;
    g_taskmgr_hotkey = 0;
    return v;
}

int keyboard_consume_ime_hotkey(void) {
    int v = g_ime_toggle_hotkey;
    g_ime_toggle_hotkey = 0;
    return v;
}

void keyboard_notify_alt_tab(int reverse) {
    g_alt_tab_steps += reverse ? -1 : 1;
    keyboard_update_ime_chord(true);
}

int keyboard_consume_alt_tab(void) {
    int v = g_alt_tab_steps;
    g_alt_tab_steps = 0;
    return v;
}

void keyboard_push_virtual(int key) {
    if (key) legacy_push(key);
}
