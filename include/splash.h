#ifndef SPLASH_H
#define SPLASH_H

#include <stdbool.h>

extern bool g_verbose_boot;

void splash_run(void);
void boot_screen_tick(void);

#endif // SPLASH_H
