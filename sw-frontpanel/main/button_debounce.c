#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "button_debounce.h"

#define TAG_BUTTON_DEBOUNCE "button_debounce"
#define BUTTON_ACTIVE_LOW (false)
#define BUTTON_ACTIVE_HIGH (true)
#define ESP_INTR_FLAG_DEFAULT (0)

typedef enum
{
    BUTTON_DOWN,
    BUTTON_UP,
    BUTTON_CLICK,
    BUTTON_DOUBLE_CLICK,
    BUTTON_HOLD,
} button_state_t;

typedef struct
{
    uint8_t number;
    uint8_t gpio;
    bool active_state;
    esp_timer_handle_t debounce_timer_handle;
    esp_timer_create_args_t debounce_timer_args;
    bool debounce_timer_running;
    bool double_click_detection;
    esp_timer_handle_t double_click_timer_handle;
    esp_timer_create_args_t double_click_timer_args;
    bool double_click_timer_running;
    bool hold_detection;
    esp_timer_handle_t hold_timer_handle;
    esp_timer_create_args_t hold_timer_args;
    bool hold_timer_running;
    bool repeat_on_hold;
    esp_timer_handle_t hold_repeat_timer_handle;
    esp_timer_create_args_t hold_repeat_timer_args;
    bool hold_repeat_timer_running;
} button_t;

typedef struct
{
    uint8_t button_number;
    uint8_t button_state;
} button_event_t;

static button_t buttons[NUM_BUTTONS] = {0};
static QueueHandle_t button_event_queue = NULL;
static TaskHandle_t button_event_task_handle = NULL;

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    button_t *button = arg;
    if (!button->debounce_timer_running)
    {
        esp_timer_start_once(button->debounce_timer_handle, DEBOUNCE_PERIOD);
        button->debounce_timer_running = true;
    }
}

static void IRAM_ATTR debounce_timer_interrupt_handler(void *arg)
{
    button_t *button = arg;
    button_event_t ev = {0};
    ev.button_number = button->number;
    if (button_event_queue)
    {
        int gpio_level = gpio_get_level(button->gpio);
        if ((gpio_level == 1 && button->active_state) || (gpio_level == 0 && !button->active_state))
        {
            ev.button_state = BUTTON_DOWN;
            xQueueSendFromISR(button_event_queue, &ev, NULL);
            ev.button_state = BUTTON_CLICK;
            xQueueSendFromISR(button_event_queue, &ev, NULL);
            if (!button->hold_timer_running && button->hold_detection)
            {
                esp_timer_start_once(button->hold_timer_handle, HOLD_PERIOD);
                button->hold_timer_running = true;
            }
            if (button->double_click_detection)
            {
                if (!button->double_click_timer_running)
                {
                    esp_timer_start_once(button->double_click_timer_handle, DOUBLE_CLICK_PERIOD);
                    button->double_click_timer_running = true;
                }
                else
                {
                    ev.button_state = BUTTON_DOUBLE_CLICK;
                    xQueueSendFromISR(button_event_queue, &ev, NULL);
                    esp_timer_stop(button->double_click_timer_handle);
                    button->double_click_timer_running = false;
                }
            }
        }
        else
        {
            ev.button_state = BUTTON_UP;
            xQueueSendFromISR(button_event_queue, &ev, NULL);
            if (button->hold_timer_running)
            {
                esp_timer_stop(button->hold_timer_handle);
                button->hold_timer_running = false;
            }
            if (button->hold_repeat_timer_running)
            {
                esp_timer_stop(button->hold_repeat_timer_handle);
                button->hold_repeat_timer_running = false;
            }
        }
    }
    button->debounce_timer_running = false;
}

static void IRAM_ATTR double_click_timer_interrupt_handler(void *arg)
{
    button_t *button = arg;
    button->double_click_timer_running = false;
}

static void IRAM_ATTR hold_timer_interrupt_handler(void *arg)
{
    button_t *button = arg;
    button_event_t ev = {0};
    ev.button_number = button->number;
    if (button_event_queue)
    {
        ev.button_state = BUTTON_HOLD;
        xQueueSendFromISR(button_event_queue, &ev, NULL);
        if (button->repeat_on_hold)
        {
            ev.button_state = BUTTON_CLICK;
            xQueueSendFromISR(button_event_queue, &ev, NULL);
            if (!button->hold_repeat_timer_running && button->repeat_on_hold)
            {
                esp_timer_start_periodic(button->hold_repeat_timer_handle, HOLD_CLICK_REPEAT_PERIOD);
                button->hold_repeat_timer_running = true;
            }
        }
    }
    button->hold_timer_running = false;
}

static void IRAM_ATTR hold_repeat_timer_interrupt_handler(void *arg)
{
    button_t *button = arg;
    button_event_t ev = {0};
    ev.button_number = button->number;
    if (button_event_queue)
    {
        ev.button_state = BUTTON_CLICK;
        xQueueSendFromISR(button_event_queue, &ev, NULL);
    }
}

extern void move_up(void);
extern void move_down(void);

static void button_event_task(void *arg)
{
    button_event_t ev;
    while (true)
    {
        if (xQueueReceive(button_event_queue, &ev, portMAX_DELAY))
        {
            /*
            ESP_LOGI(TAG_BUTTON_DEBOUNCE, "button: %d state: %s", ev.button_number, ev.button_state == BUTTON_UP ? "up" : ev.button_state == BUTTON_DOWN       ? "down"
                                                                                                                      : ev.button_state == BUTTON_CLICK        ? "click"
                                                                                                                      : ev.button_state == BUTTON_DOUBLE_CLICK ? "double-click"
                                                                                                                                                               : "hold");
                                                                                                                                                               */
            /* Add code here to detect button events and call your own functions
            e.g.
            if(ev.button_number == 0 && ev.button_state = BUTTON_CLICK)
            {
                your_function();
            }
            */
            if (ev.button_number == 0 && ev.button_state == BUTTON_CLICK) {
                move_down();
            } else if (ev.button_number == 2 && ev.button_state == BUTTON_CLICK) {
                move_up();
            }
        }
    }
}

esp_err_t button_debounce_init()
{
    if (!button_event_queue)
    {
        button_event_queue = xQueueCreate(BUTTON_EVENT_QUEUE_LENGTH, sizeof(button_event_t));
    }
    else
    {
        return ESP_ERR_NOT_ALLOWED;
    }

    if (!button_event_task_handle)
    {
        xTaskCreate(button_event_task, "button_event_task", 4096, NULL, 10, &button_event_task_handle);
    }
    else
    {
        return ESP_ERR_NOT_ALLOWED;
    }

    // Repeat these code blocks as needed, once per button. Configure each option as you wish.
    buttons[0].gpio = GPIO_BUTTON_1;
    buttons[0].active_state = BUTTON_ACTIVE_LOW;
    buttons[0].double_click_detection = true;
    buttons[0].hold_detection = true;
    buttons[0].repeat_on_hold = true;

    buttons[1].gpio = GPIO_BUTTON_2;
    buttons[1].active_state = BUTTON_ACTIVE_LOW;
    buttons[1].double_click_detection = true;
    buttons[1].hold_detection = true;
    buttons[1].repeat_on_hold = true;

    buttons[2].gpio = GPIO_BUTTON_3;
    buttons[2].active_state = BUTTON_ACTIVE_LOW;
    buttons[2].double_click_detection = true;
    buttons[2].hold_detection = true;
    buttons[2].repeat_on_hold = true;

    buttons[3].gpio = GPIO_BUTTON_4;
    buttons[3].active_state = BUTTON_ACTIVE_LOW;
    buttons[3].double_click_detection = true;
    buttons[3].hold_detection = true;
    buttons[3].repeat_on_hold = true;
    // End of button configuration code blocks

    gpio_config_t io_conf = {};
    gpio_pull_mode_t pull_mode = PULL_MODE;
    io_conf.pin_bit_mask = 0;
    for (uint8_t button = 0; button < NUM_BUTTONS; button++)
    {
        io_conf.pin_bit_mask |= (1ULL << buttons[button].gpio);
    }
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = (pull_mode == GPIO_PULLUP_ONLY || pull_mode == GPIO_PULLUP_PULLDOWN);
    io_conf.pull_down_en = (pull_mode == GPIO_PULLDOWN_ONLY || pull_mode == GPIO_PULLUP_PULLDOWN);
    if (gpio_config(&io_conf) != ESP_OK)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    err = gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    if (err != ESP_OK)
    {
        return err;
    }
    for (uint8_t button = 0; button < NUM_BUTTONS; button++)
    {
        buttons[button].number = button;
        buttons[button].debounce_timer_args.callback = &debounce_timer_interrupt_handler;
        buttons[button].debounce_timer_args.arg = &buttons[button];
        buttons[button].debounce_timer_args.name = "debounce_timer";
        err = esp_timer_create(&buttons[button].debounce_timer_args, &buttons[button].debounce_timer_handle);
        if (err != ESP_OK)
        {
            return err;
        }
        if (buttons[button].double_click_detection)
        {
            buttons[button].double_click_timer_args.callback = &double_click_timer_interrupt_handler;
            buttons[button].double_click_timer_args.arg = &buttons[button];
            buttons[button].double_click_timer_args.name = "double_click_timer";
            err = esp_timer_create(&buttons[button].double_click_timer_args, &buttons[button].double_click_timer_handle);
            if (err != ESP_OK)
            {
                return err;
            }
        }
        if (buttons[button].hold_detection)
        {
            buttons[button].hold_timer_args.callback = &hold_timer_interrupt_handler;
            buttons[button].hold_timer_args.arg = &buttons[button];
            buttons[button].hold_timer_args.name = "hold_timer";
            err = esp_timer_create(&buttons[button].hold_timer_args, &buttons[button].hold_timer_handle);
            if (err != ESP_OK)
            {
                return err;
            }
            if (buttons[button].repeat_on_hold)
            {
                buttons[button].hold_repeat_timer_args.callback = &hold_repeat_timer_interrupt_handler;
                buttons[button].hold_repeat_timer_args.arg = &buttons[button];
                buttons[button].hold_repeat_timer_args.name = "hold_repeat_timer";
                err = esp_timer_create(&buttons[button].hold_repeat_timer_args, &buttons[button].hold_repeat_timer_handle);
                if (err != ESP_OK)
                {
                    return err;
                }
            }
        }

        err = gpio_isr_handler_add(buttons[button].gpio, gpio_isr_handler, &buttons[button]);
        if (err != ESP_OK)
        {
            return err;
        }
    }
    return ESP_OK;
}
