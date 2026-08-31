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

#include "gpio_handler.h"
#include "gpio_pins.h"
#include "led_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "gpio_handler";

typedef struct {
    button_config_t config;
    uint8_t id;
    bool active;
    bool last_state;
    uint32_t last_change_time;
    uint32_t press_time;
    bool waiting_for_double_click;
    esp_timer_handle_t debounce_timer;
    esp_timer_handle_t double_click_timer;
    esp_timer_handle_t long_press_timer;
    esp_timer_handle_t repeat_timer;
} button_state_t;

static button_state_t buttons[MAX_BUTTONS];
static QueueHandle_t event_queue = NULL;
static QueueHandle_t activity_queue = NULL;
static TaskHandle_t event_task_handle = NULL;
static button_event_handler_t user_handler = NULL;
static activity_event_handler_t activity_handler = NULL;
static bool handler_running = false;

// Forward declarations
static void activity_led_handler(activity_event_t *event);
static esp_err_t gpio_handler_send_activity_event_from_isr(int pin_state, BaseType_t *pxHigherPriorityTaskWoken);

static void IRAM_ATTR gpio_isr_handler(void *arg) {
    button_state_t *button = (button_state_t *)arg;
    if (button->debounce_timer) {
        esp_timer_stop(button->debounce_timer);
        esp_timer_start_once(button->debounce_timer, button->config.debounce_ms * 1000);
    }
}

static void debounce_timer_callback(void *arg) {
    button_state_t *button = (button_state_t *)arg;
    bool current_state = gpio_get_level(button->config.gpio);
    
    if (button->config.active_low) {
        current_state = !current_state;
    }
    
    if (current_state != button->last_state) {
        button_event_t event = {
            .button_id = button->id,
            .timestamp = esp_timer_get_time() / 1000
        };
        
        if (current_state) {
            // Button pressed
            event.type = BUTTON_EVENT_PRESS;
            button->press_time = event.timestamp;
            
            if (button->config.enable_long_press && button->long_press_timer) {
                esp_timer_start_once(button->long_press_timer, 
                                   button->config.long_press_ms * 1000);
            }
        } else {
            // Button released
            event.type = BUTTON_EVENT_RELEASE;
            uint32_t press_duration = event.timestamp - button->press_time;
            
            if (button->long_press_timer) {
                esp_timer_stop(button->long_press_timer);
            }
            if (button->repeat_timer) {
                esp_timer_stop(button->repeat_timer);
            }
            
            if (press_duration < button->config.long_press_ms) {
                // Short press - potential click
                if (button->config.enable_double_click) {
                    if (button->waiting_for_double_click) {
                        // Double click detected
                        button_event_t dbl_event = event;
                        dbl_event.type = BUTTON_EVENT_DOUBLE_CLICK;
                        xQueueSend(event_queue, &dbl_event, 0);
                        button->waiting_for_double_click = false;
                        if (button->double_click_timer) {
                            esp_timer_stop(button->double_click_timer);
                        }
                    } else {
                        // Start waiting for double click
                        button->waiting_for_double_click = true;
                        if (button->double_click_timer) {
                            esp_timer_start_once(button->double_click_timer,
                                               button->config.double_click_ms * 1000);
                        }
                    }
                } else {
                    // Single click
                    button_event_t click_event = event;
                    click_event.type = BUTTON_EVENT_CLICK;
                    xQueueSend(event_queue, &click_event, 0);
                }
            }
        }
        
        xQueueSend(event_queue, &event, 0);
        button->last_state = current_state;
    }
}

static void double_click_timer_callback(void *arg) {
    button_state_t *button = (button_state_t *)arg;
    if (button->waiting_for_double_click) {
        // Timeout - it was a single click
        button_event_t event = {
            .button_id = button->id,
            .type = BUTTON_EVENT_CLICK,
            .timestamp = esp_timer_get_time() / 1000
        };
        xQueueSend(event_queue, &event, 0);
        button->waiting_for_double_click = false;
    }
}

static void long_press_timer_callback(void *arg) {
    button_state_t *button = (button_state_t *)arg;
    button_event_t event = {
        .button_id = button->id,
        .type = BUTTON_EVENT_LONG_PRESS,
        .timestamp = esp_timer_get_time() / 1000
    };
    xQueueSend(event_queue, &event, 0);
    
    if (button->config.enable_repeat && button->repeat_timer) {
        esp_timer_start_periodic(button->repeat_timer, button->config.repeat_ms * 1000);
    }
}

static void repeat_timer_callback(void *arg) {
    button_state_t *button = (button_state_t *)arg;
    button_event_t event = {
        .button_id = button->id,
        .type = BUTTON_EVENT_REPEAT,
        .timestamp = esp_timer_get_time() / 1000
    };
    xQueueSend(event_queue, &event, 0);
}

static void gpio_event_task(void *arg) {
    button_event_t button_event;
    activity_event_t activity_event;

    while (handler_running) {
        // Check button queue with short timeout
        if (xQueueReceive(event_queue, &button_event, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (user_handler) {
                user_handler(&button_event);
            }
        }

        // Check activity queue (non-blocking)
        if (xQueueReceive(activity_queue, &activity_event, 0) == pdTRUE) {
            if (activity_handler) {
                activity_handler(&activity_event);
            }
        }
    }

    vTaskDelete(NULL);
}

esp_err_t gpio_handler_init(void) {
    if (event_queue != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    event_queue = xQueueCreate(BUTTON_EVENT_QUEUE_SIZE, sizeof(button_event_t));
    if (!event_queue) {
        return ESP_ERR_NO_MEM;
    }

    activity_queue = xQueueCreate(ACTIVITY_EVENT_QUEUE_SIZE, sizeof(activity_event_t));
    if (!activity_queue) {
        vQueueDelete(event_queue);
        event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    memset(buttons, 0, sizeof(buttons));
    
    // Install GPIO ISR service here so it's ready when we add buttons
    gpio_install_isr_service(0);

    // Register default activity handler for LED control
    activity_handler = activity_led_handler;

    ESP_LOGI(TAG, "GPIO handler initialized");
    return ESP_OK;
}

esp_err_t gpio_handler_add_button(uint8_t button_id, const button_config_t *config) {
    if (button_id >= MAX_BUTTONS || !config) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (buttons[button_id].active) {
        return ESP_ERR_INVALID_STATE;
    }
    
    button_state_t *button = &buttons[button_id];
    button->config = *config;
    button->id = button_id;
    button->active = true;
    button->last_state = false;
    button->waiting_for_double_click = false;
    
    // Configure GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << config->gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = config->active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = !config->active_low ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    gpio_config(&io_conf);
    
    // Create timers
    esp_timer_create_args_t timer_args = {
        .callback = debounce_timer_callback,
        .arg = button,
        .name = "debounce"
    };
    esp_timer_create(&timer_args, &button->debounce_timer);
    
    if (config->enable_double_click) {
        timer_args.callback = double_click_timer_callback;
        timer_args.name = "double_click";
        esp_timer_create(&timer_args, &button->double_click_timer);
    }
    
    if (config->enable_long_press) {
        timer_args.callback = long_press_timer_callback;
        timer_args.name = "long_press";
        esp_timer_create(&timer_args, &button->long_press_timer);
    }
    
    if (config->enable_repeat) {
        timer_args.callback = repeat_timer_callback;
        timer_args.name = "repeat";
        esp_timer_create(&timer_args, &button->repeat_timer);
    }
    
    // Add ISR handler
    gpio_isr_handler_add(config->gpio, gpio_isr_handler, button);
    
    ESP_LOGI(TAG, "Button %d added on GPIO %d", button_id, config->gpio);
    return ESP_OK;
}

esp_err_t gpio_handler_remove_button(uint8_t button_id) {
    if (button_id >= MAX_BUTTONS) {
        return ESP_ERR_INVALID_ARG;
    }
    
    button_state_t *button = &buttons[button_id];
    if (!button->active) {
        return ESP_ERR_INVALID_STATE;
    }
    
    gpio_isr_handler_remove(button->config.gpio);
    
    if (button->debounce_timer) {
        esp_timer_stop(button->debounce_timer);
        esp_timer_delete(button->debounce_timer);
    }
    if (button->double_click_timer) {
        esp_timer_stop(button->double_click_timer);
        esp_timer_delete(button->double_click_timer);
    }
    if (button->long_press_timer) {
        esp_timer_stop(button->long_press_timer);
        esp_timer_delete(button->long_press_timer);
    }
    if (button->repeat_timer) {
        esp_timer_stop(button->repeat_timer);
        esp_timer_delete(button->repeat_timer);
    }
    
    memset(button, 0, sizeof(button_state_t));
    
    ESP_LOGI(TAG, "Button %d removed", button_id);
    return ESP_OK;
}

esp_err_t gpio_handler_register_button_callback(button_event_handler_t handler) {
    user_handler = handler;
    return ESP_OK;
}

esp_err_t gpio_handler_start(void) {
    if (handler_running) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // ISR service already installed in init()
    
    handler_running = true;
    xTaskCreate(gpio_event_task, "gpio_event", 4096, NULL, 10, &event_task_handle);
    
    ESP_LOGI(TAG, "GPIO handler started");
    return ESP_OK;
}

esp_err_t gpio_handler_stop(void) {
    if (!handler_running) {
        return ESP_ERR_INVALID_STATE;
    }
    
    handler_running = false;
    
    // Wake up task to exit
    button_event_t dummy_event = {0};
    xQueueSend(event_queue, &dummy_event, 0);
    
    // Wait for task to finish
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Note: gpio_uninstall_isr_service() should be called if we're completely done
    // But keeping it installed allows for restart
    
    ESP_LOGI(TAG, "GPIO handler stopped");
    return ESP_OK;
}

button_event_t gpio_handler_get_last_event(void) {
    button_event_t event = {0};
    xQueuePeek(event_queue, &event, 0);
    return event;
}

bool gpio_handler_has_events(void) {
    return uxQueueMessagesWaiting(event_queue) > 0;
}

esp_err_t gpio_handler_clear_events(void) {
    xQueueReset(event_queue);
    return ESP_OK;
}

#ifdef CONFIG_PANEL_TEST_HOOKS
// Same enqueue the debounce, long-press and repeat timers do, so the event is
// dispatched from gpio_event_task and handle_button_event keeps its usual task
// context. Calling the handler directly from the web task would run its
// blocking host transactions on the wrong task.
esp_err_t gpio_handler_inject_event(uint8_t button_id, button_event_type_t type) {
    if (event_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (button_id >= MAX_BUTTONS) {
        return ESP_ERR_INVALID_ARG;
    }
    button_event_t event = {
        .button_id = button_id,
        .type = type,
        .timestamp = esp_timer_get_time() / 1000
    };
    // Report a full queue rather than dropping silently, so a test that sends
    // faster than the UI drains fails instead of under-counting presses.
    return xQueueSend(event_queue, &event, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}
#endif

// Activity LED handler - sets LED color based on activity pin state
static void activity_led_handler(activity_event_t *event) {
    if (!event) return;

    if (event->pin_state) {
        // Pin is HIGH - set LED to orange
        led_set_color(COLOR_ORANGE);
    } else {
        // Pin is LOW - set LED to cyan
        led_set_color(COLOR_CYAN);
    }
}

// Activity ISR handler
static void IRAM_ATTR activity_gpio_isr_handler(void* arg) {
    gpio_num_t gpio_num = (gpio_num_t)(intptr_t)arg;
    int pin_state = gpio_get_level(gpio_num);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    gpio_handler_send_activity_event_from_isr(pin_state, &xHigherPriorityTaskWoken);

    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

// Activity indicator functions
esp_err_t gpio_handler_register_activity_callback(activity_event_handler_t handler) {
    activity_handler = handler;
    return ESP_OK;
}

esp_err_t gpio_handler_configure_activity_pin(gpio_num_t gpio_num) {
    // Configure the GPIO pin as input with interrupt on both edges
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure activity pin: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Activity pin configured on GPIO%d", gpio_num);
    return ESP_OK;
}

static esp_err_t gpio_handler_send_activity_event_from_isr(int pin_state, BaseType_t *pxHigherPriorityTaskWoken) {
    if (activity_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    activity_event_t event = {
        .pin_state = pin_state
    };

    if (xQueueSendFromISR(activity_queue, &event, pxHigherPriorityTaskWoken) != pdTRUE) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t gpio_handler_install_activity_isr(gpio_num_t gpio_num) {
    esp_err_t ret = gpio_isr_handler_add(gpio_num, activity_gpio_isr_handler, (void*)(intptr_t)gpio_num);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR handler for activity pin: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Activity ISR handler installed on GPIO%d", gpio_num);
    return ESP_OK;
}