#ifndef BUTTON_HANDLER_H
#define BUTTON_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"

#define MAX_BUTTONS 8
#define BUTTON_EVENT_QUEUE_SIZE 32

typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_PRESS,
    BUTTON_EVENT_RELEASE,
    BUTTON_EVENT_CLICK,
    BUTTON_EVENT_DOUBLE_CLICK,
    BUTTON_EVENT_LONG_PRESS,
    BUTTON_EVENT_REPEAT
} button_event_type_t;

typedef struct {
    uint8_t button_id;
    button_event_type_t type;
    uint32_t timestamp;
} button_event_t;

typedef struct {
    gpio_num_t gpio;
    bool active_low;
    bool enable_double_click;
    bool enable_long_press;
    bool enable_repeat;
    uint32_t debounce_ms;
    uint32_t double_click_ms;
    uint32_t long_press_ms;
    uint32_t repeat_ms;
} button_config_t;

typedef void (*button_event_handler_t)(button_event_t *event);

esp_err_t button_handler_init(void);
esp_err_t button_handler_add_button(uint8_t button_id, const button_config_t *config);
esp_err_t button_handler_remove_button(uint8_t button_id);
esp_err_t button_handler_register_callback(button_event_handler_t handler);
esp_err_t button_handler_start(void);
esp_err_t button_handler_stop(void);
button_event_t button_handler_get_last_event(void);
bool button_handler_has_events(void);
esp_err_t button_handler_clear_events(void);

#endif