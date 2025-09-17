#include "led_driver.h"
#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "led_driver";

static led_strip_handle_t led_strip = NULL;

// Predefined colors
const rgb_color_t COLOR_OFF =     {0,   0,   0};
const rgb_color_t COLOR_RED =     {255, 0,   0};
const rgb_color_t COLOR_GREEN =   {0,   255, 0};
const rgb_color_t COLOR_BLUE =    {0,   0,   255};
const rgb_color_t COLOR_WHITE =   {255, 255, 255};
const rgb_color_t COLOR_YELLOW =  {255, 255, 0};
const rgb_color_t COLOR_CYAN =    {0,   255, 255};
const rgb_color_t COLOR_MAGENTA = {255, 0,   255};

esp_err_t led_driver_init(void) {
    if (led_strip != NULL) {
        ESP_LOGW(TAG, "LED driver already initialized");
        return ESP_OK;
    }

    // LED strip general configuration
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO_NUM,
        .max_leds = LED_STRIP_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        }
    };

    // LED strip RMT backend configuration
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_STRIP_RMT_RES_HZ,
        .mem_block_symbols = 64,
        .flags = {
            .with_dma = false,
        }
    };

    // Create LED strip object
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED strip: %s", esp_err_to_name(ret));
        return ret;
    }

    // Clear the LED strip (turn off)
    led_clear();

    ESP_LOGI(TAG, "LED driver initialized on GPIO%d with %d LEDs", 
             LED_STRIP_GPIO_NUM, LED_STRIP_LED_COUNT);
    
    return ESP_OK;
}

esp_err_t led_driver_deinit(void) {
    if (led_strip == NULL) {
        return ESP_OK;
    }

    // Clear LEDs before deinitializing
    led_clear();
    
    esp_err_t ret = led_strip_del(led_strip);
    led_strip = NULL;
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "LED driver deinitialized");
    } else {
        ESP_LOGE(TAG, "Failed to deinitialize LED driver: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t led_set_color(rgb_color_t color) {
    return led_set_rgb(color.r, color.g, color.b);
}

esp_err_t led_set_rgb(uint8_t r, uint8_t g, uint8_t b) {
    if (led_strip == NULL) {
        ESP_LOGE(TAG, "LED driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = led_strip_set_pixel(led_strip, 0, r, g, b);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set LED color: %s", esp_err_to_name(ret));
        return ret;
    }

    return led_refresh();
}

esp_err_t led_clear(void) {
    if (led_strip == NULL) {
        ESP_LOGE(TAG, "LED driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = led_strip_clear(led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear LED strip: %s", esp_err_to_name(ret));
        return ret;
    }

    return led_refresh();
}

esp_err_t led_refresh(void) {
    if (led_strip == NULL) {
        ESP_LOGE(TAG, "LED driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = led_strip_refresh(led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to refresh LED strip: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t led_demo_rainbow(uint32_t duration_ms) {
    if (led_strip == NULL) {
        ESP_LOGE(TAG, "LED driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t step_ms = 50;  // 50ms per color step
    const uint32_t steps = duration_ms / step_ms;
    
    ESP_LOGI(TAG, "Starting rainbow demo for %lu ms", duration_ms);

    for (uint32_t i = 0; i < steps; i++) {
        // Generate rainbow color using HSV to RGB conversion
        float hue = (float)(i * 360) / steps;  // 0-360 degrees
        float saturation = 1.0f;
        float value = 0.3f;  // Reduced brightness to avoid being too bright
        
        // Convert HSV to RGB
        float c = value * saturation;
        float x = c * (1 - fabsf(fmodf(hue / 60.0f, 2) - 1));
        float m = value - c;
        
        float r_prime, g_prime, b_prime;
        
        if (hue < 60) {
            r_prime = c; g_prime = x; b_prime = 0;
        } else if (hue < 120) {
            r_prime = x; g_prime = c; b_prime = 0;
        } else if (hue < 180) {
            r_prime = 0; g_prime = c; b_prime = x;
        } else if (hue < 240) {
            r_prime = 0; g_prime = x; b_prime = c;
        } else if (hue < 300) {
            r_prime = x; g_prime = 0; b_prime = c;
        } else {
            r_prime = c; g_prime = 0; b_prime = x;
        }
        
        uint8_t r = (uint8_t)((r_prime + m) * 255);
        uint8_t g = (uint8_t)((g_prime + m) * 255);
        uint8_t b = (uint8_t)((b_prime + m) * 255);
        
        esp_err_t ret = led_set_rgb(r, g, b);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set rainbow color at step %lu", i);
        }
        
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }

    // Turn off LED after demo
    led_clear();
    
    ESP_LOGI(TAG, "Rainbow demo completed");
    return ESP_OK;
}

esp_err_t led_flash_color(rgb_color_t color, uint32_t duration_ms) {
    if (led_strip == NULL) {
        ESP_LOGE(TAG, "LED driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Flashing color R:%d G:%d B:%d for %lu ms", 
             color.r, color.g, color.b, duration_ms);

    // Turn on
    esp_err_t ret = led_set_color(color);
    if (ret != ESP_OK) {
        return ret;
    }

    // Wait
    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    // Turn off
    return led_clear();
}

// Activity indication functions
esp_err_t led_activity_read(void) {
    return led_set_color(COLOR_GREEN);
}

esp_err_t led_activity_write(void) {
    return led_set_color(COLOR_RED);
}

esp_err_t led_activity_idle(void) {
    return led_clear();
}
