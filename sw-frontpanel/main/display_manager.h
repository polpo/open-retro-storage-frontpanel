#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "u8g2.h"
#include "esp_err.h"
#include "esp_timer.h"

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
} display_manager_t;

esp_err_t display_manager_init(display_manager_t *display);
esp_err_t display_manager_clear(display_manager_t *display);
esp_err_t display_manager_request_update(display_manager_t *display);
esp_err_t display_manager_update(display_manager_t *display);
esp_err_t display_manager_draw_bitmap(display_manager_t *display, uint8_t x, uint8_t y, uint8_t width, uint8_t height, const uint8_t *bitmap);
esp_err_t display_manager_draw_text(display_manager_t *display, uint8_t x, uint8_t y, const char *text);
esp_err_t display_manager_set_font(display_manager_t *display, const uint8_t *font);
esp_err_t display_manager_set_draw_color(display_manager_t *display, uint8_t color);
esp_err_t display_manager_draw_box(display_manager_t *display, uint8_t x, uint8_t y, uint8_t width, uint8_t height);
esp_err_t display_manager_draw_frame(display_manager_t *display, uint8_t x, uint8_t y, uint8_t width, uint8_t height);
esp_err_t display_manager_wake(display_manager_t *display);

#define DISPLAY_DIM_TIMEOUT_MS (30 * 1000)    // 30 seconds
#define DISPLAY_OFF_TIMEOUT_MS (120 * 1000)   // 2 minutes

#endif