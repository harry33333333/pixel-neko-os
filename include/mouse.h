#ifndef MOUSE_H
#define MOUSE_H

typedef struct {
    int x;
    int y;
    int buttons;
    int pressed;
    int released;
    int tick;
} mouse_button_event_t;

void mouse_init(void);
void mouse_handle_byte(unsigned char d);
void mouse_get(int* x, int* y, int* btn);
int mouse_take_button_event(mouse_button_event_t* event);
void mouse_draw(unsigned char* vga);
void mouse_reset(unsigned char* vga);
void mouse_invalidate(void);

extern int mouse_speed;
void mouse_set_speed(int s);
int mouse_get_speed(void);

void mouse_handle_usb_report(int buttons, int dx, int dy, int dz);
void mouse_handle_usb_absolute(int buttons, int x, int y);
void mouse_notify_usb_detached(void);

#endif
