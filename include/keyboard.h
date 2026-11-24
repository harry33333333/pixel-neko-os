#ifndef _KEYBOARD_H
#define _KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

void keyboard_init(void);
void keyboard_handle_byte(uint8_t scancode);
int keyboard_get_key(void);
int keyboard_get_ctrl(void);
int keyboard_get_shift(void);
int keyboard_get_alt(void);
void keyboard_set_usb_modifiers(int ctrl, int shift, int alt);
void keyboard_notify_taskmgr_hotkey(void);
int keyboard_consume_taskmgr_hotkey(void);
int keyboard_consume_ime_hotkey(void);
void keyboard_notify_alt_tab(int reverse);
int keyboard_consume_alt_tab(void);
void keyboard_note_usb_activity(int has_keys);
void keyboard_push_virtual(int key);
uint32_t keyboard_get_drop_count(void);

#endif /* _KEYBOARD_H */
