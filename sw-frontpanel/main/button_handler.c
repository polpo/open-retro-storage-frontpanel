#include "button_handler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "button_handler";

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
static TaskHandle_t event_task_handle = NULL;
static button_event_handler_t user_handler = NULL;
static bool handler_running = false;

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

static void button_event_task(void *arg) {
    button_event_t event;
    
    while (handler_running) {
        if (xQueueReceive(event_queue, &event, portMAX_DELAY) == pdTRUE) {
            if (user_handler) {
                user_handler(&event);
            }
        }
    }
    
    vTaskDelete(NULL);
}

esp_err_t button_handler_init(void) {
    if (event_queue != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    event_queue = xQueueCreate(BUTTON_EVENT_QUEUE_SIZE, sizeof(button_event_t));
    if (!event_queue) {
        return ESP_ERR_NO_MEM;
    }
    
    memset(buttons, 0, sizeof(buttons));
    
    // Install GPIO ISR service here so it's ready when we add buttons
    gpio_install_isr_service(0);
    
    ESP_LOGI(TAG, "Button handler initialized");
    return ESP_OK;
}

esp_err_t button_handler_add_button(uint8_t button_id, const button_config_t *config) {
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

esp_err_t button_handler_remove_button(uint8_t button_id) {
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

esp_err_t button_handler_register_callback(button_event_handler_t handler) {
    user_handler = handler;
    return ESP_OK;
}

esp_err_t button_handler_start(void) {
    if (handler_running) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // ISR service already installed in init()
    
    handler_running = true;
    xTaskCreate(button_event_task, "button_event", 4096, NULL, 10, &event_task_handle);
    
    ESP_LOGI(TAG, "Button handler started");
    return ESP_OK;
}

esp_err_t button_handler_stop(void) {
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
    
    ESP_LOGI(TAG, "Button handler stopped");
    return ESP_OK;
}

button_event_t button_handler_get_last_event(void) {
    button_event_t event = {0};
    xQueuePeek(event_queue, &event, 0);
    return event;
}

bool button_handler_has_events(void) {
    return uxQueueMessagesWaiting(event_queue) > 0;
}

esp_err_t button_handler_clear_events(void) {
    xQueueReset(event_queue);
    return ESP_OK;
}