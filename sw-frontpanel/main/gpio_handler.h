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

#ifndef GPIO_HANDLER_H
#define GPIO_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"

#define MAX_BUTTONS 8
#define BUTTON_EVENT_QUEUE_SIZE 32
#define ACTIVITY_EVENT_QUEUE_SIZE 10

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

// Activity indicator event structure
typedef struct {
    int pin_state;
} activity_event_t;

typedef void (*activity_event_handler_t)(activity_event_t *event);

// GPIO handler initialization and control
esp_err_t gpio_handler_init(void);
esp_err_t gpio_handler_start(void);
esp_err_t gpio_handler_stop(void);

// Button functions
esp_err_t gpio_handler_add_button(uint8_t button_id, const button_config_t *config);
esp_err_t gpio_handler_remove_button(uint8_t button_id);
esp_err_t gpio_handler_register_button_callback(button_event_handler_t handler);
button_event_t gpio_handler_get_last_event(void);
bool gpio_handler_has_events(void);
esp_err_t gpio_handler_clear_events(void);

// Activity indicator functions
esp_err_t gpio_handler_register_activity_callback(activity_event_handler_t handler);
esp_err_t gpio_handler_configure_activity_pin(gpio_num_t gpio_num);
esp_err_t gpio_handler_install_activity_isr(gpio_num_t gpio_num);

#endif