#ifndef BUTTON_DEBOUNCE_H
#define BUTTON_DEBOUNCE_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#define NUM_BUTTONS (4)                     // Number of buttons or switches to monitor
#define GPIO_BUTTON_1 (4)                  // GPIO number of button 1
#define GPIO_BUTTON_2 (5)                  // GPIO number of button 2
#define GPIO_BUTTON_3 (6)                  // GPIO number of button 2
#define GPIO_BUTTON_4 (7)                  // GPIO number of button 2
#define PULL_MODE GPIO_FLOATING             // No internal pull-up resistors are used - means external pull-up or pull-down resistors should be used
#define DEBOUNCE_PERIOD (10000)             // 10ms debounce period
#define DOUBLE_CLICK_PERIOD (500000)        // 500ms timeout to trigger a double click event
#define HOLD_PERIOD (500000)               // 2s button hold trigger
#define HOLD_CLICK_REPEAT_PERIOD (50000)   // 200ms delay between repeated clicks when button is held
#define BUTTON_EVENT_QUEUE_LENGTH (10)

esp_err_t button_debounce_init();

#endif
