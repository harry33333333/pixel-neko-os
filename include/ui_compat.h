#ifndef UI_COMPAT_H
#define UI_COMPAT_H

#include <stdbool.h>

void ui_compat_init(void);
void ui_desktop_start(void);
void ui_desktop_poll(void);
bool ui_install_mode_requested(void);
void ui_compat_set_install_mode(bool enabled);

#endif
