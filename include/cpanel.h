#ifndef CPANEL_H
#define CPANEL_H

void cpanel_init(void);
void cpanel_draw(unsigned char* v, int wx, int wy, int ww, int wh);
void cpanel_click(unsigned char* v, int mx, int my, int wx, int wy, int ww, int wh);

#endif