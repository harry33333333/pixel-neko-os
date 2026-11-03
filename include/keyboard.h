#ifndef _KEYBOARD_H
#define _KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

void keyboard_init(void);
void keyboard_handle_byte(uint8_t scancode);
int keyboard_get_key(void);
int keyboard_get_ctrl(void);
void keyboard_push_virtual(int key);

#endif /* _KEYBOARD_H */
