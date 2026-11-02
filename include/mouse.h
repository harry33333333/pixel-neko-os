#ifndef _MOUSE_H
#define _MOUSE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int x;
    int y;
    int buttons;
    int pressed;
    int released;
    int tick;
} mouse_button_event_t;

void mouse_init(void);
void mouse_handle_byte(uint8_t byte);
void mouse_get(int* x, int* y, int* buttons);
int mouse_take_button_event(mouse_button_event_t* event);
int mouse_get_speed(void);
void mouse_set_speed(int speed);

#endif /* _MOUSE_H */
