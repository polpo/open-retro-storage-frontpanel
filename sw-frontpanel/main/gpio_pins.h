// SPDX-License-Identifier: GPL-2.0-only
//
//  Copyright (C) 2025  Ian Scott
//
//  This program is free software; you can redistribute it and/or modify it
//  under the terms of the GNU General Public License (as published by the
//  Free Software Foundation) version 2, dated June 1991.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License along
//  with this program; if not, see <https://www.gnu.org/licenses/>.

#pragma once

#include "driver/gpio.h"
#include "sdkconfig.h"

#ifdef CONFIG_PANEL_BOARD_C3_SUPERMINI

// ESP32-C3 SuperMini (DIY, web UI only).
//
// That board brings out GPIO0-10, GPIO20 and GPIO21 and nothing else: GPIO18
// and GPIO19 are wired to its USB-C data lines, and GPIO11-17 are the module's
// SPI flash. Three of the remaining pins are spoken for by the board itself -
// GPIO8 drives the onboard blue LED, GPIO9 is the BOOT button, GPIO2 carries
// the strapping pull-up - and GPIO20/21 are the UART console. That leaves
// eight: GPIO0, 1, 3, 4, 5, 6, 7 and 10.
//
// Both host transports get their own pins, so one image runs on every main
// board. That spends all eight, which is why this board has no OLED
// (CONFIG_PANEL_NO_DISPLAY) and no navigation buttons
// (CONFIG_PANEL_NO_BUTTONS) - the web UI drives it instead.
//
// The SPI pins deliberately avoid the SPI2 IO_MUX set (MISO 2, CLK 6, MOSI 7,
// CS 10). IO_MUX would allow 80 MHz against the GPIO matrix's 40, and the host
// link runs at 10 - so there is nothing to win, and IO_MUX MISO would land on
// GPIO2, whose strapping level a host driving the line at reset could hold low.

// Host interface, I2C (BlueSCSI v2): GPIO3 -> host GPIO16,
// GPIO10 -> host GPIO17, plus 2.2k pull-ups to 3V3 on both.
#define PIN_SDA         GPIO_NUM_3
#define PIN_SCL         GPIO_NUM_10

// Host interface, SPI (BlueSCSI Ultra/Ultra Wide, PicoIDE).
//
// PicoIDE's 12-pin panel ribbon. RX and TX are named from the PicoIDE side,
// so they cross. A ground sits between every signal; tie them all on a long
// ribbon to keep the 10 MHz clock quiet.
//
//   1, 2            3V3       3V3 (the panel runs off this)
//   3, 5, 7, 9, 11  GND       GND
//   4               SPI_RX    GPIO7  (MOSI)
//   6               SPI_TX    GPIO5  (MISO)
//   8               SPI_CLK   GPIO6
//   10              SPI_CS    GPIO4
//   12              ACT_LED   GPIO0
#define PIN_SPI_CLK     GPIO_NUM_6
#define PIN_SPI_MOSI    GPIO_NUM_7
#define PIN_SPI_MISO    GPIO_NUM_5
#define PIN_HOST_CS     GPIO_NUM_4

// Activity indicator input (from main board). Unconnected on most DIY builds,
// so gpio_handler pulls it up - a floating pin would storm the edge ISR.
#define PIN_ACT_IN      GPIO_NUM_0

// LED output (WS2812B addressable LED strip), optional.
#define PIN_LED_OUT     GPIO_NUM_1

// UART interface (USB-C console)
#define PIN_UART_TX     GPIO_NUM_21

// No OLED on this board. These stay defined so the display paths still
// compile; CONFIG_PANEL_NO_DISPLAY means they are never driven.
#define PIN_OLED_CS     GPIO_NUM_NC
#define PIN_OLED_DC     GPIO_NUM_NC

#else

// OLED Display control pins (SPI mode)
#define PIN_OLED_CS     GPIO_NUM_0   // Chip Select for SPI OLED
#define PIN_OLED_DC     GPIO_NUM_1   // Data/Command for SPI OLED

// SPI interface pins (shared by OLED display and future SPI devices)
#define PIN_SPI_MISO    GPIO_NUM_2   // Master In Slave Out (not used by OLED)
#define PIN_SPI_CLK     GPIO_NUM_6   // SPI Clock
#define PIN_SPI_MOSI    GPIO_NUM_7   // Master Out Slave In (OLED data)

// Activity indicator input (from main board)
#define PIN_ACT_IN      GPIO_NUM_20

// Navigation button pins
#define PIN_NAV_UP      GPIO_NUM_4
#define PIN_NAV_RIGHT   GPIO_NUM_9
#define PIN_NAV_DOWN    GPIO_NUM_5
#define PIN_NAV_LEFT    GPIO_NUM_8

// Host interface chip select
#define PIN_HOST_CS     GPIO_NUM_10

// LED output (WS2812B addressable LED strip)
#define PIN_LED_OUT     GPIO_NUM_18

// I2C interface pins (for communication with main board)
#define PIN_SCL         GPIO_NUM_19  // I2C Clock
#define PIN_SDA         GPIO_NUM_3  // I2C Data

// UART interface
#define PIN_UART_TX     GPIO_NUM_21

#endif // CONFIG_PANEL_BOARD_C3_SUPERMINI

// LED strip configuration
#define LED_STRIP_GPIO_NUM  PIN_LED_OUT
