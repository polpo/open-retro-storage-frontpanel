#include "display_manager.h"
#include "u8g2_esp32_hal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "display_manager";

static void dim_timer_callback(void *arg);
static void off_timer_callback(void *arg);

#define PIN_SDA GPIO_NUM_0
#define PIN_SCL GPIO_NUM_1
#define I2C_HW_ADDR 0x3C
#define MIN_UPDATE_INTERVAL_MS 16  // ~60 FPS max

esp_err_t display_manager_init(display_manager_t *display) {
    if (!display) {
        return ESP_ERR_INVALID_ARG;
    }

    u8g2_esp32_hal_t u8g2_esp32_hal = U8G2_ESP32_HAL_DEFAULT;
    u8g2_esp32_hal.bus.i2c.sda = PIN_SDA;
    u8g2_esp32_hal.bus.i2c.scl = PIN_SCL;
    u8g2_esp32_hal_init(u8g2_esp32_hal);

    u8g2_Setup_sh1107_i2c_64x128_f(&display->u8g2, U8G2_R3, 
                                   u8g2_esp32_i2c_byte_cb, 
                                   u8g2_esp32_gpio_and_delay_cb);

    u8g2_InitDisplay(&display->u8g2);
    u8g2_ClearDisplay(&display->u8g2);
    u8g2_SetPowerSave(&display->u8g2, 0);
    u8g2_SetContrast(&display->u8g2, 255); // Ensure full brightness at startup
    ESP_LOGI(TAG, "Display initialized with full contrast (255)");

    display->initialized = true;
    display->needs_update = false;
    display->last_update_ms = 0;
    display->min_update_interval_ms = MIN_UPDATE_INTERVAL_MS;
    display->last_activity_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    display->state = DISPLAY_STATE_FULL_BRIGHTNESS;

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

    // Start the dim timer
    esp_timer_start_once(display->dim_timer, DISPLAY_DIM_TIMEOUT_MS * 1000);

    ESP_LOGI(TAG, "Display initialized successfully");
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

    u8g2_SendBuffer(&display->u8g2);
    display->needs_update = false;
    display->last_update_ms = current_ms;

    return ESP_OK;
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

static void dim_timer_callback(void *arg) {
    display_manager_t *display = (display_manager_t *)arg;
    if (display && display->initialized && display->state == DISPLAY_STATE_FULL_BRIGHTNESS) {
        ESP_LOGI(TAG, "Dimming display after 30s idle - setting contrast to 25");
        display->state = DISPLAY_STATE_DIMMED;
        u8g2_SetContrast(&display->u8g2, 25); // Try much lower value
        
        // Start the off timer
        esp_timer_start_once(display->off_timer, (DISPLAY_OFF_TIMEOUT_MS - DISPLAY_DIM_TIMEOUT_MS) * 1000);
    }
}

static void off_timer_callback(void *arg) {
    display_manager_t *display = (display_manager_t *)arg;
    if (display && display->initialized && display->state == DISPLAY_STATE_DIMMED) {
        ESP_LOGI(TAG, "Turning off display after 2min idle");
        display->state = DISPLAY_STATE_OFF;
        u8g2_SetPowerSave(&display->u8g2, 1); // Turn off display
    }
}

esp_err_t display_manager_wake(display_manager_t *display) {
    if (!display || !display->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    bool was_off = (display->state == DISPLAY_STATE_OFF);
    
    // Stop any running timers
    esp_timer_stop(display->dim_timer);
    esp_timer_stop(display->off_timer);
    
    if (display->state != DISPLAY_STATE_FULL_BRIGHTNESS) {
        ESP_LOGI(TAG, "Waking display to full brightness - setting contrast to 255");
        display->state = DISPLAY_STATE_FULL_BRIGHTNESS;
        u8g2_SetPowerSave(&display->u8g2, 0);  // Wake up display
        u8g2_SetContrast(&display->u8g2, 255); // Full brightness
        display->needs_update = true;
    }

    // Restart the dim timer
    esp_timer_start_once(display->dim_timer, DISPLAY_DIM_TIMEOUT_MS * 1000);

    // Return whether the display was off (caller should ignore button action if true)
    return was_off ? ESP_ERR_NOT_FINISHED : ESP_OK;
}
