#ifndef _INPUT_H
#define _INPUT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define INPUT_QUEUE_SIZE 256

#define INPUT_KEY_ESCAPE      0x1B
#define INPUT_KEY_BACKSPACE   0x08
#define INPUT_KEY_TAB         0x09
#define INPUT_KEY_ENTER       0x0D
#define INPUT_KEY_UP          0x1000
#define INPUT_KEY_DOWN        0x1001
#define INPUT_KEY_LEFT        0x1002
#define INPUT_KEY_RIGHT       0x1003
#define INPUT_KEY_HOME        0x1004
#define INPUT_KEY_END         0x1005
#define INPUT_KEY_PAGE_UP     0x1006
#define INPUT_KEY_PAGE_DOWN   0x1007
#define INPUT_KEY_DELETE      0x1008

#define INPUT_MOUSE_LEFT      0x01
#define INPUT_MOUSE_RIGHT     0x02
#define INPUT_MOUSE_MIDDLE    0x04

typedef enum {
    INPUT_KEY_DOWN = 0,
    INPUT_KEY_UP,
    INPUT_MOUSE_MOVE,
    INPUT_MOUSE_BUTTON,
    INPUT_MOUSE_SCROLL
} input_event_type_t;

typedef struct {
    input_event_type_t type;
    uint32_t code;
    int32_t dx;
    int32_t dy;
    uint32_t buttons;
    int32_t wheel;
    uint64_t timestamp;
} input_event_t;

void input_manager_init(void);
bool input_push_event(const input_event_t* event);
bool input_poll_event(input_event_t* event);
size_t input_get_queue_depth(void);
uint64_t input_get_event_count(void);
uint64_t input_get_dropped_count(void);

void input_emit_key(uint32_t code, bool pressed);
void input_emit_mouse_move(int32_t dx, int32_t dy,
                           uint32_t buttons, int32_t wheel);
void input_emit_mouse_button(uint32_t buttons);

void ps2_input_init(void);
void ps2_irq_handler(void);

#endif /* _INPUT_H */
