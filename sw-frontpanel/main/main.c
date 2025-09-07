#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"

#include "display_manager.h"
#include "button_handler.h"
#include "menu_system.h"
#include "i2c_slave_comm.h"
#include "ui_screens.h"

static const char *TAG = "main";

// Global instances
static display_manager_t display;
static menu_t main_menu;
static menu_t disc_menu;
static i2c_slave_comm_t slave_comm;
static screen_type_t current_screen = SCREEN_SPLASH;

// Dynamic disc list - populated from I2C communication
static bool disc_list_loaded = false;
static char current_disc_name[64] = "No disc loaded";

// Forward declarations
static esp_err_t refresh_disc_list(void);

static menu_item_t main_menu_items[] = {
    {.text = "Select Disc", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "Eject Disc", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "Settings", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "System Info", .action = MENU_ACTION_CUSTOM, .selectable = true},
};

// Button event handler
static void handle_button_event(button_event_t *event) {
    if (!event) return;
    
    ESP_LOGI(TAG, "Button %d event: %d", event->button_id, event->type);
    
    // Wake display on any button activity
    // If display was off, wake returns ESP_ERR_NOT_FINISHED and we should ignore the button action
    esp_err_t wake_result = display_manager_wake(&display);
    if (wake_result == ESP_ERR_NOT_FINISHED) {
        ESP_LOGI(TAG, "Display was off - ignoring button action, just waking up");
        return; // Don't process the button action, just wake up
    }
    
    // Handle navigation based on current screen
    switch (current_screen) {
        case SCREEN_MAIN_MENU:
            switch (event->button_id) {
                case 0: // Up button (North)
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_up(&main_menu);
                        ui_draw_menu(&display, &main_menu);
                    }
                    break;
                case 2: // Down button (South)
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_down(&main_menu);
                        ui_draw_menu(&display, &main_menu);
                    }
                    break;
                case 1: // Select button (East)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        uint32_t selected = menu_get_selected_index(&main_menu);
                        if (selected == 0) { // "Select Disc"
                            // Refresh disc list before showing it
                            if (!disc_list_loaded && slave_comm.initialized) {
                                ESP_LOGI(TAG, "Loading disc list...");
                                refresh_disc_list();
                            }
                            current_screen = SCREEN_DISC_LIST;
                            ui_draw_menu(&display, &disc_menu);
                        } else if (selected == 1) { // "Eject Disc"
                            if (slave_comm.initialized) {
                                esp_err_t ret = i2c_slave_eject_disc(&slave_comm);
                                if (ret == ESP_OK) {
                                    strncpy(current_disc_name, "No disc loaded", sizeof(current_disc_name) - 1);
                                    current_disc_name[sizeof(current_disc_name) - 1] = '\0';
                                    ui_draw_status_bar(&display, current_disc_name);
                                    ESP_LOGI(TAG, "Disc ejected");
                                } else {
                                    ESP_LOGW(TAG, "Failed to eject disc: %s", esp_err_to_name(ret));
                                }
                            }
                        } else if (selected == 3) { // "System Info"
                            current_screen = SCREEN_INFO;
                            char info_text[128];
                            snprintf(info_text, sizeof(info_text), 
                                "PicoIDE Front Panel\nFW: v0.1.0\nESP32-C3\nI2C: %s",
                                slave_comm.initialized ? "Connected" : "Disconnected");
                            ui_draw_info_screen(&display, "System Info", info_text);
                        }
                    }
                    break;
                case 3: // Back button (West)
                    // Already at main menu - no action needed
                    break;
            }
            break;
            
        case SCREEN_DISC_LIST:
            switch (event->button_id) {
                case 0: // Up button (North)
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_up(&disc_menu);
                        ui_draw_menu(&display, &disc_menu);
                    }
                    break;
                case 2: // Down button (South)
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_down(&disc_menu);
                        ui_draw_menu(&display, &disc_menu);
                    }
                    break;
                case 1: // Select button (East)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        uint32_t selected = menu_get_selected_index(&disc_menu);
                        menu_item_t *selected_item = menu_get_selected_item(&disc_menu);
                        if (selected_item) {
                            ESP_LOGI(TAG, "Selected disc: %s", selected_item->text);
                            // Send command to slave device
                            if (slave_comm.initialized) {
                                esp_err_t ret = i2c_slave_select_disc(&slave_comm, selected);
                                if (ret == ESP_OK) {
                                    strncpy(current_disc_name, selected_item->text, sizeof(current_disc_name) - 1);
                                    current_disc_name[sizeof(current_disc_name) - 1] = '\0';
                                } else {
                                    ESP_LOGW(TAG, "Failed to select disc via I2C: %s", esp_err_to_name(ret));
                                }
                            }
                            // Go back to main menu
                            current_screen = SCREEN_MAIN_MENU;
                            ui_draw_menu(&display, &main_menu);
                            ui_draw_status_bar(&display, current_disc_name);
                        }
                    }
                    break;
                case 3: // Back button (West)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        current_screen = SCREEN_MAIN_MENU;
                        ui_draw_menu(&display, &main_menu);
                    }
                    break;
            }
            break;
            
        case SCREEN_INFO:
            if (event->type == BUTTON_EVENT_CLICK && event->button_id == 3) {
                // Back button - return to main menu
                current_screen = SCREEN_MAIN_MENU;
                ui_draw_menu(&display, &main_menu);
            }
            break;
            
        default:
            break;
    }
}

// Display update task
static void display_update_task(void *pvParameters) {
    while (1) {
        // Update display if needed
        if (display.initialized && display.needs_update) {
            display_manager_update(&display);
        }
        vTaskDelay(pdMS_TO_TICKS(16)); // 60 FPS to match display manager rate
    }
}

// Function to refresh disc list from I2C slave
static esp_err_t refresh_disc_list(void) {
    if (!slave_comm.initialized) {
        ESP_LOGW(TAG, "I2C slave comm not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Clear current disc list
    menu_clear_items(&disc_menu);
    
    // Get disc count from slave
    uint32_t disc_count = 0;
    esp_err_t ret = i2c_slave_get_disc_count(&slave_comm, &disc_count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get disc count: %s", esp_err_to_name(ret));
        // Add fallback message
        menu_add_item(&disc_menu, "No discs available", MENU_ACTION_SELECT, NULL, NULL);
        return ret;
    }
    
    ESP_LOGI(TAG, "Found %lu discs on slave", disc_count);
    
    if (disc_count == 0) {
        menu_add_item(&disc_menu, "No discs available", MENU_ACTION_SELECT, NULL, NULL);
        return ESP_OK;
    }

    // Get info for each disc
    for (uint32_t i = 0; i < disc_count && i < MENU_MAX_ITEMS; i++) {
        disc_info_t disc_info;
        ESP_LOGI(TAG, "Requesting disc info for index %lu", i);
        ret = i2c_slave_get_disc_info(&slave_comm, i, &disc_info);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Disc %lu: %s (%.1f MB, type %d)", i, disc_info.name, 
                    disc_info.size / 1000000.0f, disc_info.type);
            menu_add_item(&disc_menu, disc_info.name, MENU_ACTION_SELECT, NULL, NULL);
        } else {
            ESP_LOGW(TAG, "Failed to get info for disc %lu: %s", i, esp_err_to_name(ret));
            char fallback_name[32];
            snprintf(fallback_name, sizeof(fallback_name), "Disc %lu (error)", i);
            menu_add_item(&disc_menu, fallback_name, MENU_ACTION_SELECT, NULL, NULL);
        }
    }
    
    disc_list_loaded = true;
    ESP_LOGI(TAG, "Disc list refreshed successfully");
    return ESP_OK;
}

// I2C communication task
static void i2c_comm_task(void *pvParameters) {
    // Wait a bit for system to stabilize
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    while (1) {
        if (slave_comm.initialized) {
            // Periodically check slave status
            i2c_status_t status;
            esp_err_t ret = i2c_slave_get_status(&slave_comm, &status);
            if (ret == ESP_OK) {
                ESP_LOGD(TAG, "Slave status: 0x%02X", status);
            } else {
                ESP_LOGW(TAG, "Failed to get slave status: %s", esp_err_to_name(ret));
            }
            
            // Refresh disc list if not loaded or periodically
            static uint32_t refresh_counter = 0;
            if (!disc_list_loaded || (refresh_counter % 12) == 0) { // Every ~60 seconds
                ESP_LOGI(TAG, "Refreshing disc list...");
                refresh_disc_list();
            }
            refresh_counter++;
        }
        vTaskDelay(pdMS_TO_TICKS(5000)); // Check every 5 seconds
    }
}


void app_main(void) {
    ESP_LOGI(TAG, "PicoIDE Front Panel Starting...");
    
    // Initialize display
    esp_err_t ret = display_manager_init(&display);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display: %s", esp_err_to_name(ret));
        return;
    }
    
    // Show splash screen
    ui_show_splash_screen(&display, 2000);
    
    // Initialize button handler
    ret = button_handler_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize button handler: %s", esp_err_to_name(ret));
        return;
    }
    
    // Configure buttons with repeat for up/down navigation
    button_config_t nav_button_config = {
        .active_low = true,
        .enable_double_click = false,
        .enable_long_press = true,
        .enable_repeat = true,
        .debounce_ms = 50,
        .double_click_ms = 300,
        .long_press_ms = 300,  // Start repeating after 300ms
        .repeat_ms = 80        // Repeat every 80ms
    };
    
    // Configure buttons without repeat for select/back
    button_config_t action_button_config = {
        .active_low = true,
        .enable_double_click = false,
        .enable_long_press = false,
        .enable_repeat = false,
        .debounce_ms = 50,
        .double_click_ms = 300,
        .long_press_ms = 1000,
        .repeat_ms = 100
    };
    
    // Add navigation buttons with repeat (up/down)
    nav_button_config.gpio = GPIO_NUM_4;
    button_handler_add_button(0, &nav_button_config); // Up (North)
    
    nav_button_config.gpio = GPIO_NUM_6;
    button_handler_add_button(2, &nav_button_config); // Down (South)
    
    // Add action buttons without repeat (select/back)
    action_button_config.gpio = GPIO_NUM_5;
    button_handler_add_button(1, &action_button_config); // Select (East)
    
    action_button_config.gpio = GPIO_NUM_7;
    button_handler_add_button(3, &action_button_config); // Back (West)
    
    // Register button event handler
    button_handler_register_callback(handle_button_event);
    button_handler_start();
    
    // Initialize menus
    menu_init(&main_menu, 8);
    menu_set_items(&main_menu, main_menu_items, 
                  sizeof(main_menu_items) / sizeof(main_menu_items[0]));
    
    menu_init(&disc_menu, 8);
    // Disc menu will be populated dynamically from I2C communication
    
    // Initialize I2C slave communication
    // Using port 0, slave address 0x50 (different from display at 0x3C)
    ret = i2c_slave_comm_init(&slave_comm, I2C_NUM_0, 0x50, GPIO_NUM_0, GPIO_NUM_1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize I2C slave comm: %s", esp_err_to_name(ret));
        // Continue anyway - slave might not be connected
    } else {
        // Skip bus scan for cleaner I2C traffic analysis
        ESP_LOGI(TAG, "I2C slave comm initialized successfully");
        
        // Manually fetch disc list at startup
        ESP_LOGI(TAG, "Fetching disc list at startup...");
        esp_err_t disc_ret = refresh_disc_list();
        if (disc_ret == ESP_OK) {
            ESP_LOGI(TAG, "Disc list loaded successfully at startup");
        } else {
            ESP_LOGW(TAG, "Failed to load disc list at startup: %s", esp_err_to_name(disc_ret));
        }
    }
    
    // Switch to main menu
    current_screen = SCREEN_MAIN_MENU;
    ui_draw_menu(&display, &main_menu);
    ui_draw_status_bar(&display, current_disc_name);
    
    // Create display update task
    xTaskCreate(display_update_task, "display_update", 4096, NULL, 5, NULL);
    
    // Create I2C communication task (commented out to reduce debug noise)
    // xTaskCreate(i2c_comm_task, "i2c_comm", 4096, NULL, 3, NULL);
    
    ESP_LOGI(TAG, "System initialized successfully");
    
    // Main loop can be used for other tasks or left empty
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
