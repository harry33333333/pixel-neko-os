/*
 * Pixel Neko Next - USB Hub Class Definitions
 *
 * Derived from FreeBSD sys/dev/usb/usb_hub.h
 * Copyright (c) 2008 Hans Petter Selasky <hselasky@FreeBSD.org>
 * Copyright (c) 1998 Lennart Augustsson <lennart@augustsson.net>
 * Copyright (c) 2026 Pixel Neko Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef USB_HUB_H
#define USB_HUB_H

#include "usb_core.h"

#define USB_CLASS_HUB 0x09

/* Hub class descriptor type */
#define USB_DT_HUB    0x29

/* Port status bits (wPortStatus) */
#define HUB_PORT_CONNECTION     0x0001
#define HUB_PORT_ENABLE         0x0002
#define HUB_PORT_SUSPEND        0x0004
#define HUB_PORT_OVER_CURRENT   0x0008
#define HUB_PORT_RESET          0x0010
#define HUB_PORT_POWER          0x0100
#define HUB_PORT_LOW_SPEED      0x0200
#define HUB_PORT_HIGH_SPEED     0x0400

/* Port change bits (wPortChange) */
#define HUB_C_PORT_CONNECTION   0x0001
#define HUB_C_PORT_ENABLE       0x0002
#define HUB_C_PORT_SUSPEND      0x0004
#define HUB_C_PORT_OVER_CURRENT 0x0008
#define HUB_C_PORT_RESET        0x0010

/* Hub Class Port Feature Selectors */
#define HUB_FEATURE_PORT_ENABLE       1
#define HUB_FEATURE_PORT_SUSPEND      2
#define HUB_FEATURE_PORT_RESET        4
#define HUB_FEATURE_PORT_POWER        8
#define HUB_FEATURE_C_CONNECTION      16
#define HUB_FEATURE_C_ENABLE          17
#define HUB_FEATURE_C_SUSPEND         18
#define HUB_FEATURE_C_OVER_CURRENT    19
#define HUB_FEATURE_C_RESET           20

int usb_hub_probe(usb_device_t* hub);

#endif /* USB_HUB_H */
