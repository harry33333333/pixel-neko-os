/*
 * Pixel Neko Next - VMware VMMouse compatibility interface
 * Copyright (c) 2026 Pixel Neko Project
 * Licensed under the MIT License.
 */

#ifndef VMWARE_MOUSE_H
#define VMWARE_MOUSE_H

int  vmware_mouse_init(void);
void vmware_mouse_poll(void);
int  vmware_mouse_active(void);

#endif /* VMWARE_MOUSE_H */
