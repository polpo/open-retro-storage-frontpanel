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

#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "u8g2.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef enum {
    DISPLAY_STATE_FULL_BRIGHTNESS,
    DISPLAY_STATE_DIMMED,
    DISPLAY_STATE_OFF
} display_state_t;

typedef struct {
    u8g2_t u8g2;
    bool needs_update;
    bool initialized;
    uint32_t last_update_ms;
    uint32_t min_update_interval_ms;
    uint32_t last_activity_ms;
    display_state_t state;
    esp_timer_handle_t dim_timer;
    esp_timer_handle_t off_timer;
    SemaphoreHandle_t spi_mutex;
} display_manager_t;

esp_err_t display_manager_init(display_manager_t *display);
esp_err_t display_manager_clear(display_manager_t *display);
esp_err_t display_manager_request_update(display_manager_t *display);
esp_err_t display_manager_update(display_manager_t *display);
esp_err_t display_manager_draw_bitmap(display_manager_t *display, uint8_t x, uint8_t y, uint8_t width, uint8_t height, const uint8_t *bitmap);
esp_err_t display_manager_draw_text(display_manager_t *display, uint8_t x, uint8_t y, const char *text);
esp_err_t display_manager_draw_glyph(display_manager_t *display, uint8_t x, uint8_t y, uint16_t glyph);
esp_err_t display_manager_set_font(display_manager_t *display, const uint8_t *font);
esp_err_t display_manager_set_draw_color(display_manager_t *display, uint8_t color);
esp_err_t display_manager_draw_box(display_manager_t *display, uint8_t x, uint8_t y, uint8_t width, uint8_t height);
esp_err_t display_manager_draw_frame(display_manager_t *display, uint8_t x, uint8_t y, uint8_t width, uint8_t height);
esp_err_t display_manager_wake(display_manager_t *display);

#define DISPLAY_DIM_TIMEOUT_MS (120 * 1000)    // 2 minutes
#define DISPLAY_OFF_TIMEOUT_MS (600 * 1000)   // 10 minutes

#endif
