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

#include "display_manager.h"
#include "u8g2_esp32_hal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "driver/spi_master.h"
#include "nvs.h"
#include "gpio_pins.h"
#include "toaster_sprite.h"
#include <string.h>

static const char *TAG = "display_manager";

#define DISPLAY_NVS_NAMESPACE   "display_cfg"
#define DISPLAY_NVS_KEY_IDLE    "idle_mode"
#define DISPLAY_NVS_KEY_TIMEOUT "idle_timeout"
#define DISPLAY_WIDTH  128
#define DISPLAY_HEIGHT 64
#define SPRITE_X_PER_Y (-2)  // Matches PicoPOST's invSlope = -2.0
#define DVD_LOGO_TICK_MS 33  // ~30 fps; matches the display task's iteration rate
// Clamp the configured idle timeout so it stays meaningfully below the off
// timeout (and above 5s for sanity).
#define DISPLAY_IDLE_TIMEOUT_MIN_MS (5 * 1000)
#define DISPLAY_IDLE_TIMEOUT_MAX_MS (DISPLAY_OFF_TIMEOUT_MS - 30 * 1000)

static void dim_timer_callback(void *arg);
static void off_timer_callback(void *arg);
static void enter_dimmed(display_manager_t *display);
static void enter_screensaver(display_manager_t *display);
static void enter_off(display_manager_t *display);
static void schedule_idle_timer(display_manager_t *display);
static void load_idle_config_from_nvs(display_manager_t *display);
static void save_idle_mode_to_nvs(display_idle_mode_t mode);
static void save_idle_timeout_to_nvs(uint32_t timeout_ms);
static void respawn_sprite(sprite_state_t *sprite);
static bool spawn_zone_busy(sprite_state_t *sprites, int self_idx);
static void step_sprite(sprite_state_t *sprites, int idx, uint32_t now_ms);

#define MIN_UPDATE_INTERVAL_MS 16  // ~60 FPS max

esp_err_t display_manager_init(display_manager_t *display) {
    if (!display) {
        return ESP_ERR_INVALID_ARG;
    }

    // Configure u8g2 HAL to use the existing SPI bus (initialized in main.c)
    u8g2_esp32_hal_t u8g2_esp32_hal = U8G2_ESP32_HAL_DEFAULT;
    u8g2_esp32_hal.bus.spi.clk = PIN_SPI_CLK;
    u8g2_esp32_hal.bus.spi.mosi = PIN_SPI_MOSI;
    u8g2_esp32_hal.bus.spi.cs = PIN_OLED_CS;
    u8g2_esp32_hal.bus.spi.host = SPI2_HOST;           // Explicitly set SPI host
    u8g2_esp32_hal.bus.spi.use_existing_bus = true;    // Use the bus initialized in main.c
    u8g2_esp32_hal.dc = PIN_OLED_DC;
    u8g2_esp32_hal.reset = U8G2_ESP32_HAL_UNDEFINED;   // No reset pin
    u8g2_esp32_hal_init(u8g2_esp32_hal);

    // Initialize SH1107 in SPI mode with 4-wire SPI
    // Using full frame buffer (f) for smooth updates
    u8g2_Setup_sh1107_64x128_f(&display->u8g2, U8G2_R3, 
                                u8g2_esp32_spi_byte_cb, 
                                u8g2_esp32_gpio_and_delay_cb);

    u8g2_InitDisplay(&display->u8g2);
    u8g2_ClearDisplay(&display->u8g2);
    u8g2_SetPowerSave(&display->u8g2, 0);
    u8g2_SetContrast(&display->u8g2, 255); // Ensure full brightness at startup
    ESP_LOGI(TAG, "Display initialized with full contrast (255)");

    display->initialized = true;
    display->needs_update = false;
    display->needs_full_redraw = false;
    display->last_update_ms = 0;
    display->min_update_interval_ms = MIN_UPDATE_INTERVAL_MS;
    display->last_activity_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    display->state = DISPLAY_STATE_FULL_BRIGHTNESS;
    memset(&display->saver, 0, sizeof(display->saver));
    for (int i = 0; i < SCREENSAVER_MAX_SPRITES; i++) {
        display->saver.sprites[i].fully_hidden = true;
    }
    load_idle_config_from_nvs(display);

    // Create mutex for SPI bus protection
    display->spi_mutex = xSemaphoreCreateMutex();
    if (!display->spi_mutex) {
        ESP_LOGE(TAG, "Failed to create SPI mutex");
        return ESP_ERR_NO_MEM;
    }

    // Create power management timers
    esp_timer_create_args_t dim_timer_args = {
        .callback = &dim_timer_callback,
        .arg = display,
        .name = "display_dim"
    };
    esp_timer_create(&dim_timer_args, &display->dim_timer);

    esp_timer_create_args_t off_timer_args = {
        .callback = &off_timer_callback,
        .arg = display,
        .name = "display_off"
    };
    esp_timer_create(&off_timer_args, &display->off_timer);

    schedule_idle_timer(display);

    ESP_LOGI(TAG, "Display initialized successfully (idle_mode=%d)", display->idle_mode);
    return ESP_OK;
}

esp_err_t display_manager_clear(display_manager_t *display) {
    if (!display || !display->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    u8g2_ClearBuffer(&display->u8g2);
    return ESP_OK;
}

esp_err_t display_manager_request_update(display_manager_t *display) {
    if (!display || !display->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    display->needs_update = true;
    return ESP_OK;
}

esp_err_t display_manager_update(display_manager_t *display) {
    if (!display || !display->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (display->state == DISPLAY_STATE_OFF) {
        return ESP_OK;  // Don't update when display is off
    }

    if (!display->needs_update) {
        return ESP_OK;
    }

    uint32_t current_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (current_ms - display->last_update_ms < display->min_update_interval_ms) {
        return ESP_OK;  // Skip update if too soon
    }

    // Protect SPI access with mutex
    if (xSemaphoreTake(display->spi_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        u8g2_SendBuffer(&display->u8g2);
        xSemaphoreGive(display->spi_mutex);

        display->needs_update = false;
        display->last_update_ms = current_ms;
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "Failed to acquire SPI mutex for update");
        return ESP_ERR_TIMEOUT;
    }
}

esp_err_t display_manager_draw_bitmap(display_manager_t *display, uint8_t x, uint8_t y, 
                                      uint8_t width, uint8_t height, const uint8_t *bitmap) {
    if (!display || !display->initialized || !bitmap) {
        return ESP_ERR_INVALID_ARG;
    }

    u8g2_DrawXBMP(&display->u8g2, x, y, width, height, bitmap);
    return ESP_OK;
}

esp_err_t display_manager_draw_text(display_manager_t *display, uint8_t x, uint8_t y, const char *text) {
    if (!display || !display->initialized || !text) {
        return ESP_ERR_INVALID_ARG;
    }

    u8g2_DrawStr(&display->u8g2, x, y, text);
    return ESP_OK;
}

esp_err_t display_manager_draw_glyph(display_manager_t *display, uint8_t x, uint8_t y, uint16_t glyph) {
    if (!display || !display->initialized || !glyph) {
        return ESP_ERR_INVALID_ARG;
    }

    u8g2_DrawGlyph(&display->u8g2, x, y, glyph);
    return ESP_OK;
}

esp_err_t display_manager_set_font(display_manager_t *display, const uint8_t *font) {
    if (!display || !display->initialized || !font) {
        return ESP_ERR_INVALID_ARG;
    }

    u8g2_SetFont(&display->u8g2, font);
    return ESP_OK;
}

esp_err_t display_manager_set_draw_color(display_manager_t *display, uint8_t color) {
    if (!display || !display->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    u8g2_SetDrawColor(&display->u8g2, color);
    return ESP_OK;
}

esp_err_t display_manager_draw_box(display_manager_t *display, uint8_t x, uint8_t y, 
                                   uint8_t width, uint8_t height) {
    if (!display || !display->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    u8g2_DrawBox(&display->u8g2, x, y, width, height);
    return ESP_OK;
}

esp_err_t display_manager_draw_frame(display_manager_t *display, uint8_t x, uint8_t y, 
                                     uint8_t width, uint8_t height) {
    if (!display || !display->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    u8g2_DrawFrame(&display->u8g2, x, y, width, height);
    return ESP_OK;
}

static void enter_dimmed(display_manager_t *display) {
    ESP_LOGI(TAG, "Entering DIMMED state");
    display->state = DISPLAY_STATE_DIMMED;
    if (xSemaphoreTake(display->spi_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        u8g2_SetContrast(&display->u8g2, 25);
        xSemaphoreGive(display->spi_mutex);
    }
    esp_timer_start_once(display->off_timer,
                         (uint64_t)(DISPLAY_OFF_TIMEOUT_MS - display->idle_timeout_ms) * 1000);
}

static void enter_screensaver(display_manager_t *display) {
    ESP_LOGI(TAG, "Entering SCREENSAVER state (mode=%d)", display->idle_mode);
    display->state = DISPLAY_STATE_SCREENSAVER;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    display->saver.frame = 0;
    display->saver.last_step_ms = now - TOASTER_FRAME_MS;  // Force immediate first draw

    if (display->idle_mode == DISPLAY_IDLE_MODE_DVD_LOGO) {
        // Random starting position somewhere inside the screen, random diagonal direction.
        display->saver.logo.x = (int16_t)(esp_random() % (DISPLAY_WIDTH - DVD_LOGO_WIDTH));
        display->saver.logo.y = (int16_t)(esp_random() % (DISPLAY_HEIGHT - DVD_LOGO_HEIGHT));
        display->saver.logo.dx = (esp_random() & 1) ? 1 : -1;
        display->saver.logo.dy = (esp_random() & 1) ? 1 : -1;
    } else {
        // Toasters: stagger initial spawns 0..2000 ms.
        for (int i = 0; i < SCREENSAVER_MAX_SPRITES; i++) {
            display->saver.sprites[i].fully_hidden = true;
            display->saver.sprites[i].respawn_at_ms = now + (esp_random() % 2000);
        }
    }
}

static void enter_off(display_manager_t *display) {
    ESP_LOGI(TAG, "Entering OFF state");
    bool was_screensaver = (display->state == DISPLAY_STATE_SCREENSAVER);
    display->state = DISPLAY_STATE_OFF;
    if (xSemaphoreTake(display->spi_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        u8g2_SetPowerSave(&display->u8g2, 1);
        xSemaphoreGive(display->spi_mutex);
    }
    // If we'd been drawing toasters, the buffer/GDDRAM has stale content;
    // when we wake, the underlying screen needs to be redrawn.
    if (was_screensaver) {
        display->needs_full_redraw = true;
    }
}

static void schedule_idle_timer(display_manager_t *display) {
    if (display->idle_mode == DISPLAY_IDLE_MODE_NONE) {
        // Skip the intermediate stage: jump straight to OFF after the full timeout
        esp_timer_start_once(display->off_timer, (uint64_t)DISPLAY_OFF_TIMEOUT_MS * 1000);
    } else {
        esp_timer_start_once(display->dim_timer, (uint64_t)display->idle_timeout_ms * 1000);
    }
}

static void dim_timer_callback(void *arg) {
    display_manager_t *display = (display_manager_t *)arg;
    if (!display || !display->initialized) return;
    if (display->state != DISPLAY_STATE_FULL_BRIGHTNESS) return;

    switch (display->idle_mode) {
        case DISPLAY_IDLE_MODE_DIM:
            enter_dimmed(display);
            break;
        case DISPLAY_IDLE_MODE_TOASTERS:
        case DISPLAY_IDLE_MODE_DVD_LOGO:
            enter_screensaver(display);
            esp_timer_start_once(display->off_timer,
                                 (uint64_t)(DISPLAY_OFF_TIMEOUT_MS - display->idle_timeout_ms) * 1000);
            break;
        case DISPLAY_IDLE_MODE_NONE:
            // Should not have armed dim_timer in NONE mode, but be safe
            enter_off(display);
            break;
    }
}

static void off_timer_callback(void *arg) {
    display_manager_t *display = (display_manager_t *)arg;
    if (!display || !display->initialized) return;
    if (display->state == DISPLAY_STATE_OFF) return;
    enter_off(display);
}

esp_err_t display_manager_wake(display_manager_t *display) {
    if (!display || !display->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    bool was_idle = (display->state != DISPLAY_STATE_FULL_BRIGHTNESS);
    bool was_screensaver = (display->state == DISPLAY_STATE_SCREENSAVER);

    esp_timer_stop(display->dim_timer);
    esp_timer_stop(display->off_timer);

    if (was_idle) {
        ESP_LOGI(TAG, "Waking display to full brightness");
        display->state = DISPLAY_STATE_FULL_BRIGHTNESS;

        if (xSemaphoreTake(display->spi_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            u8g2_SetPowerSave(&display->u8g2, 0);
            u8g2_SetContrast(&display->u8g2, 255);
            xSemaphoreGive(display->spi_mutex);
        }

        // The screensaver overwrote the buffer; the off-after-screensaver path
        // already flagged needs_full_redraw via enter_off.
        if (was_screensaver) {
            display->needs_full_redraw = true;
        }

        display->needs_update = true;
    }

    schedule_idle_timer(display);

    // Caller should suppress the triggering action whenever wake actually
    // brought the display back from any idle state — the button only restores
    // brightness / cancels the screensaver, it does not also navigate.
    return was_idle ? ESP_ERR_NOT_FINISHED : ESP_OK;
}

esp_err_t display_manager_set_idle_mode(display_manager_t *display, display_idle_mode_t mode) {
    if (!display) return ESP_ERR_INVALID_ARG;
    if (mode != DISPLAY_IDLE_MODE_DIM &&
        mode != DISPLAY_IDLE_MODE_TOASTERS &&
        mode != DISPLAY_IDLE_MODE_DVD_LOGO &&
        mode != DISPLAY_IDLE_MODE_NONE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (display->idle_mode == mode) return ESP_OK;

    display->idle_mode = mode;
    save_idle_mode_to_nvs(mode);

    // Restart the idle timer chain so the new mode takes effect from now.
    if (display->initialized && display->state == DISPLAY_STATE_FULL_BRIGHTNESS) {
        esp_timer_stop(display->dim_timer);
        esp_timer_stop(display->off_timer);
        schedule_idle_timer(display);
    }
    return ESP_OK;
}

display_idle_mode_t display_manager_get_idle_mode(display_manager_t *display) {
    if (!display) return DISPLAY_IDLE_MODE_DIM;
    return display->idle_mode;
}

esp_err_t display_manager_set_idle_timeout(display_manager_t *display, uint32_t timeout_ms) {
    if (!display) return ESP_ERR_INVALID_ARG;
    if (timeout_ms < DISPLAY_IDLE_TIMEOUT_MIN_MS) timeout_ms = DISPLAY_IDLE_TIMEOUT_MIN_MS;
    if (timeout_ms > DISPLAY_IDLE_TIMEOUT_MAX_MS) timeout_ms = DISPLAY_IDLE_TIMEOUT_MAX_MS;
    if (display->idle_timeout_ms == timeout_ms) return ESP_OK;

    display->idle_timeout_ms = timeout_ms;
    save_idle_timeout_to_nvs(timeout_ms);

    if (display->initialized && display->state == DISPLAY_STATE_FULL_BRIGHTNESS) {
        esp_timer_stop(display->dim_timer);
        esp_timer_stop(display->off_timer);
        schedule_idle_timer(display);
    }
    return ESP_OK;
}

uint32_t display_manager_get_idle_timeout(display_manager_t *display) {
    if (!display) return DISPLAY_IDLE_TIMEOUT_DEFAULT_MS;
    return display->idle_timeout_ms;
}

bool display_manager_screensaver_active(display_manager_t *display) {
    return display && display->state == DISPLAY_STATE_SCREENSAVER;
}

// Roughly 1-in-5 sprites are toast; the rest are animated toasters.
#define TOAST_SPAWN_PROBABILITY_NUM 1
#define TOAST_SPAWN_PROBABILITY_DEN 5

static void respawn_sprite(sprite_state_t *sprite) {
    // Offsets up to DISPLAY_WIDTH/2 enter from the top; the higher range puts
    // the sprite far enough off-right that it first appears along the right edge.
    sprite->offset = (int16_t)(esp_random() %
                               ((DISPLAY_WIDTH / 2) + (DISPLAY_HEIGHT - TOASTER_HEIGHT)));
    sprite->y = -TOASTER_HEIGHT;
    sprite->x = (int16_t)((sprite->y - sprite->offset) * SPRITE_X_PER_Y);
    sprite->fully_hidden = false;
    sprite->kind = (esp_random() % TOAST_SPAWN_PROBABILITY_DEN) < TOAST_SPAWN_PROBABILITY_NUM
                       ? SPRITE_KIND_TOAST
                       : SPRITE_KIND_TOASTER;
}

// True if any *other* visible sprite still occupies the top spawn zone
// (y < 0). Spawning while that's true would vertically overlap the newcomer,
// and since all sprites move in parallel, that overlap would never resolve.
static bool spawn_zone_busy(sprite_state_t *sprites, int self_idx) {
    for (int j = 0; j < SCREENSAVER_MAX_SPRITES; j++) {
        if (j == self_idx) continue;
        if (!sprites[j].fully_hidden && sprites[j].y < 0) return true;
    }
    return false;
}

static void step_sprite(sprite_state_t *sprites, int idx, uint32_t now_ms) {
    sprite_state_t *sprite = &sprites[idx];
    if (sprite->fully_hidden) {
        if ((int32_t)(now_ms - sprite->respawn_at_ms) >= 0) {
            if (spawn_zone_busy(sprites, idx)) {
                // Recheck on the next animation tick.
                sprite->respawn_at_ms = now_ms + TOASTER_FRAME_MS;
            } else {
                respawn_sprite(sprite);
            }
        }
        return;
    }

    sprite->y++;
    sprite->x = (int16_t)((sprite->y - sprite->offset) * SPRITE_X_PER_Y);

    bool offscreen = (sprite->x > DISPLAY_WIDTH || sprite->y > DISPLAY_HEIGHT) ||
                     (sprite->x < -TOASTER_WIDTH || sprite->y < -TOASTER_HEIGHT);
    if (offscreen) {
        sprite->fully_hidden = true;
        sprite->respawn_at_ms = now_ms + 500 + (esp_random() % 2500);  // 0.5..3.0 s
    }
}

static void tick_toasters(display_manager_t *display, uint32_t now) {
    display->saver.frame = (display->saver.frame + 1) % TOASTER_FRAMES;

    for (int i = 0; i < SCREENSAVER_MAX_SPRITES; i++) {
        step_sprite(display->saver.sprites, i, now);
    }

    u8g2_ClearBuffer(&display->u8g2);
    const uint8_t *toaster_bytes = toaster_frames[display->saver.frame];
    for (int i = 0; i < SCREENSAVER_MAX_SPRITES; i++) {
        sprite_state_t *s = &display->saver.sprites[i];
        if (s->fully_hidden) continue;
        const uint8_t *bytes = (s->kind == SPRITE_KIND_TOAST) ? toast_frame : toaster_bytes;
        u8g2_DrawBitmap(&display->u8g2, s->x, s->y,
                        TOASTER_BYTES_PER_ROW, TOASTER_HEIGHT, bytes);
    }
}

static void tick_bouncing_logo(display_manager_t *display) {
    bouncing_logo_state_t *l = &display->saver.logo;
    l->x += l->dx;
    l->y += l->dy;
    if (l->x <= 0)                                 { l->x = 0; l->dx = 1; }
    if (l->x >= DISPLAY_WIDTH - DVD_LOGO_WIDTH)    { l->x = DISPLAY_WIDTH - DVD_LOGO_WIDTH; l->dx = -1; }
    if (l->y <= 0)                                 { l->y = 0; l->dy = 1; }
    if (l->y >= DISPLAY_HEIGHT - DVD_LOGO_HEIGHT)  { l->y = DISPLAY_HEIGHT - DVD_LOGO_HEIGHT; l->dy = -1; }

    u8g2_ClearBuffer(&display->u8g2);
    u8g2_DrawBitmap(&display->u8g2, l->x, l->y,
                    DVD_LOGO_BYTES_PER_ROW, DVD_LOGO_HEIGHT, dvd_logo);
}

void display_manager_screensaver_tick(display_manager_t *display) {
    if (!display || !display->initialized) return;
    if (display->state != DISPLAY_STATE_SCREENSAVER) return;

    bool is_dvd = (display->idle_mode == DISPLAY_IDLE_MODE_DVD_LOGO);
    uint32_t step_ms = is_dvd ? DVD_LOGO_TICK_MS : TOASTER_FRAME_MS;

    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if ((int32_t)(now - display->saver.last_step_ms) < step_ms) {
        return;
    }
    display->saver.last_step_ms = now;

    if (is_dvd) {
        tick_bouncing_logo(display);
    } else {
        tick_toasters(display, now);
    }
    display->needs_update = true;
}

static void load_idle_config_from_nvs(display_manager_t *display) {
    display->idle_mode = DISPLAY_IDLE_MODE_DIM;
    display->idle_timeout_ms = DISPLAY_IDLE_TIMEOUT_DEFAULT_MS;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(DISPLAY_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "No saved display config (using defaults)");
        return;
    }

    uint8_t mode_stored = 0;
    if (nvs_get_u8(handle, DISPLAY_NVS_KEY_IDLE, &mode_stored) == ESP_OK &&
        mode_stored <= (uint8_t)DISPLAY_IDLE_MODE_NONE) {
        display->idle_mode = (display_idle_mode_t)mode_stored;
    }

    uint32_t timeout_stored = 0;
    if (nvs_get_u32(handle, DISPLAY_NVS_KEY_TIMEOUT, &timeout_stored) == ESP_OK) {
        if (timeout_stored < DISPLAY_IDLE_TIMEOUT_MIN_MS) {
            timeout_stored = DISPLAY_IDLE_TIMEOUT_MIN_MS;
        } else if (timeout_stored > DISPLAY_IDLE_TIMEOUT_MAX_MS) {
            timeout_stored = DISPLAY_IDLE_TIMEOUT_MAX_MS;
        }
        display->idle_timeout_ms = timeout_stored;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Loaded display cfg: idle_mode=%u timeout_ms=%lu",
             display->idle_mode, (unsigned long)display->idle_timeout_ms);
}

static void save_idle_mode_to_nvs(display_idle_mode_t mode) {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(DISPLAY_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for display config: %s", esp_err_to_name(ret));
        return;
    }
    nvs_set_u8(handle, DISPLAY_NVS_KEY_IDLE, (uint8_t)mode);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Saved idle_mode=%u", mode);
}

static void save_idle_timeout_to_nvs(uint32_t timeout_ms) {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(DISPLAY_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for display config: %s", esp_err_to_name(ret));
        return;
    }
    nvs_set_u32(handle, DISPLAY_NVS_KEY_TIMEOUT, timeout_ms);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Saved idle_timeout_ms=%lu", (unsigned long)timeout_ms);
}
