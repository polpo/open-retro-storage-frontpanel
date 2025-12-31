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

#ifndef LED_DRIVER_H
#define LED_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "gpio_pins.h"

#define LED_STRIP_LED_COUNT 1

// RMT resolution for LED strip (10MHz for precise timing)
#define LED_STRIP_RMT_RES_HZ (10 * 1000 * 1000)

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_color_t;

// Predefined colors
extern const rgb_color_t COLOR_OFF;
extern const rgb_color_t COLOR_RED;
extern const rgb_color_t COLOR_GREEN;
extern const rgb_color_t COLOR_BLUE;
extern const rgb_color_t COLOR_WHITE;
extern const rgb_color_t COLOR_YELLOW;
extern const rgb_color_t COLOR_ORANGE;
extern const rgb_color_t COLOR_CYAN;
extern const rgb_color_t COLOR_MAGENTA;

// LED driver functions
esp_err_t led_driver_init(void);
esp_err_t led_driver_deinit(void);
esp_err_t led_set_color(rgb_color_t color);
esp_err_t led_set_rgb(uint8_t r, uint8_t g, uint8_t b);
esp_err_t led_clear(void);
esp_err_t led_refresh(void);

// Effect functions
esp_err_t led_demo_rainbow(uint32_t duration_ms);
esp_err_t led_flash_color(rgb_color_t color, uint32_t duration_ms);
esp_err_t led_start_pulse(rgb_color_t color);
esp_err_t led_stop_pulse(void);

// Activity indication functions (for future use)
esp_err_t led_activity_read(void);
esp_err_t led_activity_write(void);
esp_err_t led_activity_idle(void);

#endif // LED_DRIVER_H
