#include "keyboard.h"
#include "input.h"

static bool g_shift = false;
static bool g_ctrl = false;
static bool g_caps = false;
static bool g_extended = false;
static uint8_t g_e1_remaining = 0;

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
    uint8_t base = normal_map[scancode & 0x7F];
    if (base >= 'a' && base <= 'z') {
        return (g_shift ^ g_caps) ? (uint32_t)(base - 32) : base;
    }
    if (g_shift && shift_map[scancode & 0x7F]) {
        return shift_map[scancode & 0x7F];
    }
    return base;
}

void keyboard_init(void) {
    g_shift = false;
    g_ctrl = false;
    g_caps = false;
    g_extended = false;
    g_e1_remaining = 0;
    g_key_head = 0;
    g_key_tail = 0;
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
            case 0x1D: g_ctrl = !release; break;
            case 0x38: break;
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
                g_shift = !release;
                break;
            case 0x1D:
                g_ctrl = !release;
                break;
            case 0x38:
                break;
            case 0x3A:
                if (!release) g_caps = !g_caps;
                break;
            default:
                translated = translate(code);
                break;
        }
    }

    if (!translated) return;
    if (!release) legacy_push((int)translated);
    input_emit_key(translated, !release);
}

int keyboard_get_key(void) {
    if (g_key_head == g_key_tail) return 0;
    int key = g_key_queue[g_key_tail];
    g_key_tail = (g_key_tail + 1u) % 64u;
    return key;
}

int keyboard_get_ctrl(void) {
    return g_ctrl ? 1 : 0;
}

int keyboard_get_shift(void) {
    return g_shift ? 1 : 0;
}

void keyboard_push_virtual(int key) {
    if (key) legacy_push(key);
}
