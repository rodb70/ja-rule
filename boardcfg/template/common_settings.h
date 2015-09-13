/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * template/common_settings.h
 * Copyright (C) 2015 Simon Newton
 */

#ifndef BOARDCFG_TEMPLATE_COMMON_SETTINGS_H_
#define BOARDCFG_TEMPLATE_COMMON_SETTINGS_H_

/**
 * @file common_settings.h
 * @brief Configuration settings by both the bootloader and the main app.
 */

/*
 * @name USB
 * USB Configuration.
 * @{
 */

/**
 * @brief The power consumption of the USB device.
 *
 * Per the USB spec, this is multipled by 2 to give the current in mA.
 * e.g. 50 = 100mA, 100 = 200mA
 */
#define USB_POWER_CONSUMPTION 100

/**
 * @}
 */

#endif  // BOARDCFG_TEMPLATE_COMMON_SETTINGS_H_
