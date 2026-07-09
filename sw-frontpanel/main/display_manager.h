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
    DISPLAY_STATE_SCREENSAVER,
    DISPLAY_STATE_OFF
} display_state_t;

typedef enum {
    DISPLAY_IDLE_MODE_DIM = 0,
    DISPLAY_IDLE_MODE_TOASTERS = 1,
    DISPLAY_IDLE_MODE_DVD_LOGO = 2,
} display_idle_mode_t;

#define SCREENSAVER_MAX_SPRITES 5

typedef enum {
    SPRITE_KIND_TOASTER = 0,  // Animated, 3 frames
    SPRITE_KIND_TOAST   = 1,  // Static, single frame
} sprite_kind_t;

typedef struct {
    int16_t x;
    int16_t y;
    int16_t offset;
    bool fully_hidden;       // True while offscreen, waiting to respawn
    uint32_t respawn_at_ms;  // Absolute time to respawn when fully_hidden
    sprite_kind_t kind;
} sprite_state_t;

typedef struct {
    int16_t x, y;
    int8_t  dx, dy;          // -1 or +1
} bouncing_logo_state_t;

typedef struct {
    sprite_state_t sprites[SCREENSAVER_MAX_SPRITES];
    bouncing_logo_state_t logo;
    uint8_t frame;           // Shared animation frame for all toaster sprites
    uint32_t last_step_ms;
} screensaver_state_t;

// Called whenever the display transitions into or out of DISPLAY_STATE_OFF
// (the fully blanked state). Used by main.c to drive the LED breathing effect.
typedef void (*display_off_state_cb_t)(bool now_off, void *user_ctx);

typedef struct {
    u8g2_t u8g2;
    bool needs_update;
    bool needs_full_redraw;  // Set when waking from a state that overwrote the buffer
    bool initialized;
    uint32_t last_update_ms;
    uint32_t min_update_interval_ms;
    uint32_t last_activity_ms;
    display_state_t state;
    display_idle_mode_t idle_mode;
    uint32_t idle_timeout_ms;  // Time until intermediate (dim/screensaver) state engages
    uint32_t off_timeout_ms;   // Total time of inactivity before blanking the display
    screensaver_state_t saver;
    esp_timer_handle_t dim_timer;
    esp_timer_handle_t off_timer;
    SemaphoreHandle_t spi_mutex;
    display_off_state_cb_t off_state_cb;
    void *off_state_cb_ctx;
} display_manager_t;

esp_err_t display_manager_init(display_manager_t *display);
// Re-assert the SH1107 init sequence and repaint the current buffer. Used to
// recover the OLED when it missed its power-on init on a slow (host-only) power
// rail. Idempotent when the panel is already up.
esp_err_t display_manager_reinit(display_manager_t *display);
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

esp_err_t display_manager_set_idle_mode(display_manager_t *display, display_idle_mode_t mode);
display_idle_mode_t display_manager_get_idle_mode(display_manager_t *display);
esp_err_t display_manager_set_idle_timeout(display_manager_t *display, uint32_t timeout_ms);
uint32_t display_manager_get_idle_timeout(display_manager_t *display);
esp_err_t display_manager_set_off_timeout(display_manager_t *display, uint32_t timeout_ms);
uint32_t display_manager_get_off_timeout(display_manager_t *display);
void display_manager_set_off_state_callback(display_manager_t *display,
                                            display_off_state_cb_t cb, void *user_ctx);
bool display_manager_screensaver_active(display_manager_t *display);
void display_manager_screensaver_tick(display_manager_t *display);

#define DISPLAY_IDLE_TIMEOUT_DEFAULT_MS (120 * 1000)         // 2 minutes
#define DISPLAY_OFF_TIMEOUT_DEFAULT_MS  (10 * 60 * 1000)     // 10 minutes
#define DISPLAY_OFF_TIMEOUT_MIN_MS      (10 * 60 * 1000)     // 10 minutes
#define DISPLAY_OFF_TIMEOUT_MAX_MS      (60 * 60 * 1000)     // 1 hour
// timeout == 0 means "never"
#define DISPLAY_OFF_TIMEOUT_NEVER       (0)
#define DISPLAY_IDLE_TIMEOUT_NEVER      (0)

#endif
