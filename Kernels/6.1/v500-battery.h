/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_POWER_V500_BATTERY_H
#define _LINUX_POWER_V500_BATTERY_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/power_supply.h>

#if IS_ENABLED(CONFIG_BATTERY_LG_V500)
int v500_usb_set_budget(unsigned int ma);
void v500_usb_set_source(enum power_supply_usb_type source);
#else
static inline int v500_usb_set_budget(unsigned int ma) { return -EOPNOTSUPP; }
static inline void v500_usb_set_source(enum power_supply_usb_type source) {}
#endif
#endif
