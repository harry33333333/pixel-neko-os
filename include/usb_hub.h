#ifndef _USB_HUB_H
#define _USB_HUB_H

#include "usb_core.h"

#define USB_HUB_MAX_DEVICES 4

#define HUB_PORT_CONNECTION      0x0001U
#define HUB_PORT_ENABLE          0x0002U
#define HUB_PORT_SUSPEND         0x0004U
#define HUB_PORT_OVER_CURRENT    0x0008U
#define HUB_PORT_RESET           0x0010U
#define HUB_PORT_POWER           0x0100U
#define HUB_PORT_LOW_SPEED       0x0200U
#define HUB_PORT_HIGH_SPEED      0x0400U

#define HUB_C_PORT_CONNECTION    0x0001U
#define HUB_C_PORT_ENABLE        0x0002U
#define HUB_C_PORT_SUSPEND       0x0004U
#define HUB_C_PORT_OVER_CURRENT  0x0008U
#define HUB_C_PORT_RESET         0x0010U

#define HUB_FEATURE_PORT_ENABLE   1U
#define HUB_FEATURE_PORT_SUSPEND  2U
#define HUB_FEATURE_PORT_RESET    4U
#define HUB_FEATURE_PORT_POWER    8U
#define HUB_FEATURE_C_CONNECTION  16U
#define HUB_FEATURE_C_ENABLE      17U
#define HUB_FEATURE_C_SUSPEND     18U
#define HUB_FEATURE_C_OVER_CURRENT 19U
#define HUB_FEATURE_C_RESET       20U

int usb_hub_attach(usb_device_t* hub);
void usb_hub_detach(usb_device_t* hub);
void usb_hub_poll(void);
int usb_hub_count(void);

#endif
