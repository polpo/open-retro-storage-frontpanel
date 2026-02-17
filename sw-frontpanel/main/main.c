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

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/spi_master.h"

#include "gpio_pins.h"
#include "gpio_handler.h"
#include "display_manager.h"
#include "menu_system.h"
#include "host_comm.h"  // Modular transport layer (I2C/SPI)
#include "transport_config.h" // Transport configuration
#include "ui_screens.h"
#include "led_driver.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "interface_common.h"
#include "esp_netif_ip_addr.h"
#include "ota_manager.h"
#include "driver/gpio.h"
#include "freertos/queue.h"

static const char *TAG = "main";

// Global instances
static display_manager_t display;
static menu_t main_menu;
static menu_t disc_menu;
static menu_t wifi_menu;
static menu_t settings_menu;
static menu_t *active_menu = NULL;  // Pointer to currently displayed menu
static screen_type_t info_return_screen = SCREEN_MAIN_MENU;  // Screen to return to from SCREEN_INFO

// Menu lookup by screen type (screens without menus are NULL)
static menu_t* screen_menus[SCREEN_COUNT] = {
    [SCREEN_MAIN_MENU] = &main_menu,
    [SCREEN_DISC_LIST] = &disc_menu,
    [SCREEN_SETTINGS] = &settings_menu,
    [SCREEN_WIFI_MENU] = &wifi_menu,
};
static host_comm_t host_comm;
static wifi_manager_t wifi_manager;
static web_server_t web_server;
static interface_context_t interface_ctx;
static screen_type_t current_screen = SCREEN_SPLASH;
static ota_manager_t ota_manager;

// Dynamic disc list - populated from I2C communication
static bool disc_list_loaded = false;
static char current_disc_name[64] = "No disc loaded";
static playback_status_t current_playback_status = {0};
static bool disc_name_changed = true;  // Flag to signal title changed (resets scroll)
static bool status_needs_redraw = true;  // Flag to signal status screen needs redraw
static loaded_image_status_t current_image_status = {0};  // Full image status including directory

// Device type (IDE vs ATAPI) - affects available operations
static uint8_t current_device_type = PANEL_DEVICE_TYPE_ATAPI;  // Default to ATAPI

// Firmware status tracking
static rp2350_fw_status_t rp2350_fw_status = {0};
static panel_firmware_info_t panel_fw_info = {0};
static bool fw_status_valid = false;
static uint8_t fw_screen_selection = 0;  // 0=Panel, 1=Main board

// Forward declarations
static esp_err_t refresh_directory_list(void);
static void refresh_playback_status(void);
static void refresh_device_type(void);
static void check_firmware_status(void);
static void draw_firmware_status_screen(void);
static void trigger_panel_update(void);
static void trigger_mainboard_update(void);
static void reconnect_to_host(void);
static bool handle_comm_error(esp_err_t err);

// Activity LED handler
static void handle_activity_event(activity_event_t *event) {
    if (!event) return;

    if (!current_playback_status.disc_inserted) {
        led_set_color(COLOR_ORANGE);  // No image loaded
    } else if (event->pin_state) {
        led_set_color(COLOR_YELLOW);  // Activity
    } else {
        led_set_color(COLOR_CYAN);    // Idle with image
    }
}

static menu_item_t main_menu_items[] = {
    {.text = "Select Image", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "Eject Image", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "Settings", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "System Info", .action = MENU_ACTION_CUSTOM, .selectable = true},
};

static menu_item_t settings_menu_items[] = {
    {.text = "Firmware Update", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "WiFi", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "Display Settings", .action = MENU_ACTION_CUSTOM, .selectable = true},
};

static menu_item_t wifi_menu_items[] = {
    {.text = "WiFi Status", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "Reset WiFi", .action = MENU_ACTION_CUSTOM, .selectable = true},
};

// Button event handler
static void handle_button_event(button_event_t *event) {
    if (!event) return;

    ESP_LOGI(TAG, "Button %d event: %d", event->button_id, event->type);

    // Only CLICK events wake display and reset activity timer
    // This avoids double-processing since PRESS fires before CLICK
    if (event->type == BUTTON_EVENT_CLICK) {
        esp_err_t wake_result = display_manager_wake(&display);
        if (wake_result == ESP_ERR_NOT_FINISHED) {
            ESP_LOGI(TAG, "Display was off - ignoring button action, just waking up");
            return;
        }
    }
    
    // Handle navigation based on current screen
    switch (current_screen) {
        case SCREEN_STATUS:
            switch (event->button_id) {
                case 0: // Up button (North) - Previous image
                    if (event->type == BUTTON_EVENT_CLICK) {
                        if (host_comm.initialized && current_image_status.image_loaded) {
                            esp_err_t ret = host_comm_select_prev_image(&host_comm);
                            if (ret == ESP_OK) {
                                refresh_playback_status();
                            }
                        }
                    }
                    break;
                case 1: // Right button (East) - Go to main menu
                    if (event->type == BUTTON_EVENT_CLICK) {
                        current_screen = SCREEN_MAIN_MENU;
                    }
                    break;
                case 2: // Down button (South) - Next image
                    if (event->type == BUTTON_EVENT_CLICK) {
                        if (host_comm.initialized && current_image_status.image_loaded) {
                            esp_err_t ret = host_comm_select_next_image(&host_comm);
                            if (ret == ESP_OK) {
                                refresh_playback_status();
                            }
                        }
                    }
                    break;
                case 3: // Back button (West) - Eject (ATAPI only)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        if (current_device_type != PANEL_DEVICE_TYPE_IDE && host_comm.initialized) {
                            esp_err_t ret = host_comm_eject_image(&host_comm);
                            if (ret == ESP_OK) {
                                refresh_playback_status();
                                ESP_LOGI(TAG, "Image ejected from status screen");
                            }
                        }
                    }
                    break;
                default:
                    break;
            }
            break;

        case SCREEN_MAIN_MENU:
            switch (event->button_id) {
                case 0: // Up button (North)
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_up(&main_menu);
                    }
                    break;
                case 2: // Down button (South)
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_down(&main_menu);
                    }
                    break;
                case 1: // Select button (East)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        uint32_t selected = menu_get_selected_index(&main_menu);
                        if (selected == 0) { // "Select Image"
                            // Refresh directory list before showing it
                            if (!disc_list_loaded && host_comm.initialized) {
                                ESP_LOGI(TAG, "Loading directory list...");
                                refresh_directory_list();
                            }
                            current_screen = SCREEN_DISC_LIST;
                        } else if (selected == 1) { // "Eject Image"
                            if (current_device_type == PANEL_DEVICE_TYPE_IDE) {
                                // IDE mode: eject not supported
                                info_return_screen = SCREEN_MAIN_MENU;
                                current_screen = SCREEN_INFO;
                                ui_draw_info_screen(&display, "Not Available",
                                    "Eject is not available\nin IDE mode.\n\nUse Select Image to\nchoose a different\nhard disk image.");
                            } else if (host_comm.initialized) {
                                esp_err_t ret = host_comm_eject_image(&host_comm);
                                if (ret == ESP_OK) {
                                    ESP_LOGI(TAG, "Image ejected");
                                    current_screen = SCREEN_STATUS;
                                    refresh_playback_status();
                                } else {
                                    ESP_LOGW(TAG, "Failed to eject disc: %s", esp_err_to_name(ret));
                                    handle_comm_error(ret);
                                }
                            }
                        } else if (selected == 2) { // "Settings"
                            current_screen = SCREEN_SETTINGS;
                        } else if (selected == 3) { // "System Info"
                            info_return_screen = SCREEN_MAIN_MENU;
                            current_screen = SCREEN_INFO;
                            const esp_app_desc_t* info_app_desc = esp_app_get_description();
                            char main_ver_str[16] = "N/A";
                            if (host_comm.initialized) {
                                rp2350_fw_status_t fw_status;
                                if (host_comm_get_rp2350_fw_status(&host_comm, &fw_status) == ESP_OK) {
                                    snprintf(main_ver_str, sizeof(main_ver_str), "%lu.%lu.%lu",
                                             (fw_status.current_version >> 16) & 0xFF,
                                             (fw_status.current_version >> 8) & 0xFF,
                                             fw_status.current_version & 0xFF);
                                }
                            }
                            char info_text[160];
                            snprintf(info_text, sizeof(info_text),
                                     "%s\nPanel: v%s\nMain:  v%s\n%s: %s",
                                     PRODUCT_NAME_FULL,
                                     info_app_desc->version,
                                     main_ver_str,
                                     host_comm_get_transport_name(&host_comm),
                                     host_comm.initialized ? "Connected" : "Disconnected");
                            ui_draw_info_screen(&display, "System Info", info_text);
                        }
                    }
                    break;
                case 3: // Back button (West)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        current_screen = SCREEN_STATUS;
                        refresh_playback_status();
                    }
                    break;
            }
            break;

        case SCREEN_DISC_LIST:
            switch (event->button_id) {
                case 0: // Up button (North)
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_up(&disc_menu);
                    }
                    break;
                case 2: // Down button (South)
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_down(&disc_menu);
                    }
                    break;
                case 1: // Select button (East)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        uint32_t selected = menu_get_selected_index(&disc_menu);
                        menu_item_t *selected_item = menu_get_selected_item(&disc_menu);
                        if (selected_item) {
                            ESP_LOGI(TAG, "Selected entry: %s (menu index %lu)", selected_item->text, selected);
                            // Send command to host device
                            if (host_comm.initialized) {
                                // First menu item is ".." (parent dir = index -1)
                                // Other items are entries (index 0, 1, 2...)
                                int32_t entry_index = (selected == 0) ? -1 : (int32_t)(selected - 1);

                                // Check if this is directory navigation (".." or "[directory]")
                                bool is_directory = (selected == 0) || (selected_item->text[0] == '[');

                                esp_err_t ret = host_comm_select_entry(&host_comm, entry_index);
                                if (ret == ESP_OK) {
                                    if (is_directory) {
                                        // Directory navigation: refresh the list
                                        refresh_directory_list();
                                    } else if (current_device_type == PANEL_DEVICE_TYPE_IDE) {
                                        // IDE mode: image selected for next boot
                                        info_return_screen = SCREEN_DISC_LIST;
                                        current_screen = SCREEN_INFO;
                                        ui_draw_info_screen(&display, "Image Selected",
                                            "Image will be loaded\non next power cycle.\n\nPress back to\nreturn to browser.");
                                    } else {
                                        // ATAPI mode: image loaded, return to status screen
                                        current_screen = SCREEN_STATUS;
                                        refresh_playback_status();
                                    }
                                } else {
                                    ESP_LOGW(TAG, "Failed to select entry via host comm: %s", esp_err_to_name(ret));
                                    if (handle_comm_error(ret)) {
                                        current_screen = SCREEN_MAIN_MENU;
                                        active_menu = &main_menu;
                                    }
                                }
                            }
                        }
                    }
                    break;
                case 3: // Back button (West)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        current_screen = SCREEN_MAIN_MENU;
                    }
                    break;
            }
            break;

        case SCREEN_SETTINGS:
            switch (event->button_id) {
                case 0: // Up button (North)
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_up(&settings_menu);
                    }
                    break;
                case 2: // Down button (South)
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_down(&settings_menu);
                    }
                    break;
                case 1: // Select button (East)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        uint32_t selected = menu_get_selected_index(&settings_menu);
                        if (selected == 0) { // "Firmware Updates"
                            current_screen = SCREEN_FIRMWARE_STATUS;
                            active_menu = NULL;  // Clear before slow operation
                            check_firmware_status();
                        } else if (selected == 1) { // "WiFi Setup"
                            current_screen = SCREEN_WIFI_MENU;
                        } else if (selected == 2) { // "Display Settings"
                            ESP_LOGI(TAG, "Display settings not implemented yet");
                        }
                    }
                    break;
                case 3: // Back button (West)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        current_screen = SCREEN_MAIN_MENU;
                    }
                    break;
            }
            break;

        case SCREEN_WIFI_MENU:
            switch (event->button_id) {
                case 0: // Up button (North)
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_up(&wifi_menu);
                    }
                    break;
                case 2: // Down button (South)
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_down(&wifi_menu);
                    }
                    break;
                case 1: // Select button (East)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        uint32_t selected = menu_get_selected_index(&wifi_menu);
                        if (selected == 0) { // "WiFi Status"
                            wifi_manager_state_t state = wifi_manager_get_state(&wifi_manager);
                            char info_text[256];

                            if (state == WIFI_MANAGER_STATE_AP_MODE) {
                                // Show AP mode info with credentials
                                snprintf(info_text, sizeof(info_text),
                                    "Mode: Access Point\n"
                                    "SSID: %s\n"
                                    "Password: %s\n"
                                    "IP: 192.168.4.1",
                                    WIFI_MANAGER_AP_SSID, WIFI_MANAGER_AP_PASSWORD);
                            } else if (wifi_manager_is_connected(&wifi_manager)) {
                                // Show client mode info
                                wifi_manager_config_t cfg;
                                wifi_manager_get_config(&wifi_manager, &cfg);
                                esp_ip4_addr_t ip;
                                if (wifi_manager_get_ip_info(&wifi_manager, &ip, NULL, NULL) == ESP_OK) {
                                    snprintf(info_text, sizeof(info_text),
                                        "Mode: Client\n"
                                        "SSID: %s\n"
                                        "IP: " IPSTR,
                                        cfg.ssid, IP2STR(&ip));
                                } else {
                                    snprintf(info_text, sizeof(info_text),
                                        "Mode: Client\n"
                                        "SSID: %s\n"
                                        "IP: Unknown",
                                        cfg.ssid);
                                }
                            } else {
                                snprintf(info_text, sizeof(info_text),
                                    "Status: %s",
                                    wifi_manager_state_to_string(state));
                            }
                            info_return_screen = SCREEN_WIFI_MENU;
                            current_screen = SCREEN_INFO;
                            ui_draw_info_screen(&display, "WiFi Status", info_text);
                        } else if (selected == 1) { // "Reset WiFi"
                            ESP_LOGI(TAG, "Resetting WiFi to defaults");
                            wifi_manager_disconnect(&wifi_manager);
                            wifi_manager_clear_config(&wifi_manager);
                            wifi_manager_start_ap(&wifi_manager);
                            char info_text[256];
                            snprintf(info_text, sizeof(info_text),
                                "WiFi settings cleared.\n\n"
                                "Connect to:\n"
                                "SSID: %s\n"
                                "Pass: %s",
                                WIFI_MANAGER_AP_SSID, WIFI_MANAGER_AP_PASSWORD);
                            info_return_screen = SCREEN_WIFI_MENU;
                            current_screen = SCREEN_INFO;
                            ui_draw_info_screen(&display, "WiFi Reset", info_text);
                        }
                    }
                    break;
                case 3: // Back button (West)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        current_screen = SCREEN_SETTINGS;
                    }
                    break;
            }
            break;

        case SCREEN_INFO:
            if (event->type == BUTTON_EVENT_CLICK && event->button_id == 3) {
                // Back button - return to previous screen
                current_screen = info_return_screen;
            }
            break;

        case SCREEN_FIRMWARE_UPDATE:
            // During firmware update, ignore all button presses
            ESP_LOGI(TAG, "Firmware update in progress - ignoring button input");
            break;

        case SCREEN_FIRMWARE_STATUS:
            switch (event->button_id) {
                case 0: // Up button (North)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        if (fw_screen_selection > 0) {
                            fw_screen_selection--;
                            draw_firmware_status_screen();
                        }
                    }
                    break;
                case 2: // Down button (South)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        if (fw_screen_selection < 1) {
                            fw_screen_selection++;
                            draw_firmware_status_screen();
                        }
                    }
                    break;
                case 1: // Select button (East) - trigger update
                    if (event->type == BUTTON_EVENT_CLICK) {
                        if (fw_screen_selection == 0 && panel_fw_info.available) {
                            trigger_panel_update();
                        } else if (fw_screen_selection == 1 && rp2350_fw_status.available_version != 0) {
                            trigger_mainboard_update();
                        }
                    }
                    break;
                case 3: // Back button (West)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        current_screen = SCREEN_SETTINGS;
                        fw_status_valid = false;
                    }
                    break;
            }
            break;

        default:
            break;
    }

    // Update active_menu based on current screen (NULL for screens without menus)
    active_menu = screen_menus[current_screen];

    // Trigger redraw of active menu when switching screens
    if (active_menu) {
        active_menu->needs_redraw = true;
    }
}

// Display update task
static void display_update_task(void *pvParameters) {
    static screen_type_t last_screen = SCREEN_COUNT;  // Invalid value forces initial draw

    while (1) {
        if (display.initialized) {
            bool screen_changed = (current_screen != last_screen);
            last_screen = current_screen;

            if (current_screen == SCREEN_STATUS) {
                if (screen_changed || status_needs_redraw) {
                    // Full redraw when entering status screen or status changed
                    ui_draw_status_screen(&display, current_disc_name,
                                          &current_image_status,
                                          &current_playback_status,
                                          screen_changed || disc_name_changed);
                    disc_name_changed = false;
                    status_needs_redraw = false;
                } else {
                    // Animate status screen scrolling text
                    ui_animate_status_screen(&display);
                }
            } else if (active_menu) {
                if (screen_changed || menu_needs_redraw(active_menu)) {
                    // Full menu redraw needed (screen change, navigation, etc.)
                    ui_draw_menu(&display, active_menu);
                } else if (ui_menu_needs_animation(active_menu)) {
                    // Lightweight scrolling animation
                    ui_animate_menu(&display, active_menu);
                }
            }

            // Send buffer to display if needed
            if (display.needs_update) {
                display_manager_update(&display);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(33)); // 30 FPS for smooth animation
    }
}

// Status screen refresh task - updates playback data from host
static void status_refresh_task(void *pvParameters) {
    while (1) {
        // Only refresh if on status screen and host is initialized
        if (current_screen == SCREEN_STATUS && host_comm.initialized) {
            refresh_playback_status();
        }
        vTaskDelay(pdMS_TO_TICKS(1000)); // Update every 1 second
    }
}

// Function to refresh playback status from host
static void refresh_playback_status(void) {
    char old_disc_name[64];
    strncpy(old_disc_name, current_disc_name, sizeof(old_disc_name) - 1);
    old_disc_name[sizeof(old_disc_name) - 1] = '\0';

    if (!host_comm.initialized) {
        memset(&current_playback_status, 0, sizeof(current_playback_status));
        memset(&current_image_status, 0, sizeof(current_image_status));
        strncpy(current_disc_name, "No disc loaded", sizeof(current_disc_name) - 1);
        current_disc_name[sizeof(current_disc_name) - 1] = '\0';
        return;
    }

    esp_err_t ret = host_comm_get_playback_status(&host_comm, &current_playback_status);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get playback status: %s", esp_err_to_name(ret));
        memset(&current_playback_status, 0, sizeof(current_playback_status));
        strncpy(current_disc_name, "No disc loaded", sizeof(current_disc_name) - 1);
        current_disc_name[sizeof(current_disc_name) - 1] = '\0';
    } else if (!current_playback_status.disc_inserted) {
        strncpy(current_disc_name, "No disc loaded", sizeof(current_disc_name) - 1);
        current_disc_name[sizeof(current_disc_name) - 1] = '\0';
    } else {
        // Copy disc name from playback status
        strncpy(current_disc_name, current_playback_status.disc_name, sizeof(current_disc_name) - 1);
        current_disc_name[sizeof(current_disc_name) - 1] = '\0';
    }

    // Check if disc name actually changed (for fetching full image status and resetting scroll)
    bool name_changed = (strcmp(old_disc_name, current_disc_name) != 0);

    // Fetch loaded image status when disc name changed (reduces SPI traffic)
    if (name_changed) {
        disc_name_changed = true;  // Reset scroll animation
        ret = host_comm_get_loaded_image_status(&host_comm, &current_image_status);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to get loaded image status: %s", esp_err_to_name(ret));
            memset(&current_image_status, 0, sizeof(current_image_status));
        } else {
            current_device_type = current_image_status.device_type;
        }
    }

    // Always trigger status screen redraw after fetching status
    status_needs_redraw = true;

    // Update LED based on current image/activity state
    activity_event_t event = { .pin_state = gpio_get_level(PIN_ACT_IN) };
    handle_activity_event(&event);
}

// Function to refresh device type from host
static void refresh_device_type(void) {
    if (!host_comm.initialized) {
        return;
    }

    loaded_image_status_t status;
    esp_err_t ret = host_comm_get_loaded_image_status(&host_comm, &status);
    if (ret == ESP_OK) {
        current_device_type = status.device_type;
        ESP_LOGI(TAG, "Device type: %s",
                 current_device_type == PANEL_DEVICE_TYPE_IDE ? "IDE (Hard Disk)" : "ATAPI (CD-ROM)");
    }
}

// Function to refresh directory entry list from host
static esp_err_t refresh_directory_list(void) {
    if (!host_comm.initialized) {
        ESP_LOGW(TAG, "Host comm not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Clear current menu
    menu_clear_items(&disc_menu);

    // Get entry count from host
    uint32_t entry_count = 0;
    esp_err_t ret = host_comm_get_entry_count(&host_comm, &entry_count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get entry count: %s", esp_err_to_name(ret));
        // Add fallback message
        menu_add_item(&disc_menu, "No entries available", MENU_ACTION_SELECT, NULL, NULL);
        return ret;
    }

    ESP_LOGI(TAG, "Found %lu entries in current directory", entry_count);

    if (entry_count == 0) {
        menu_add_item(&disc_menu, "Empty directory", MENU_ACTION_SELECT, NULL, NULL);
        return ESP_OK;
    }

    // Add ".." entry to go to parent directory
    menu_add_item(&disc_menu, "..", MENU_ACTION_SELECT, NULL, NULL);

    // Get info for each entry (directories and files)
    for (uint32_t i = 0; i < entry_count && i < MENU_MAX_ITEMS - 1; i++) {
        dir_entry_info_t entry_info;
        ESP_LOGI(TAG, "Requesting entry info for index %lu", i);
        ret = host_comm_get_entry_info(&host_comm, i, &entry_info);
        if (ret == ESP_OK) {
            // Add prefix to distinguish directories from files
            char display_name[68];  // 64 + prefix + null
            if (entry_info.entry_type == 0) {  // DIRECTORY
                snprintf(display_name, sizeof(display_name), "[%s]", entry_info.name);
            } else {
                snprintf(display_name, sizeof(display_name), "%s", entry_info.name);
            }

            ESP_LOGI(TAG, "Entry %lu: %s (type=%u)", i, entry_info.name, entry_info.entry_type);
            menu_add_item(&disc_menu, display_name, MENU_ACTION_SELECT, NULL, NULL);
        } else {
            ESP_LOGW(TAG, "Failed to get info for entry %lu: %s", i, esp_err_to_name(ret));
            char fallback_name[32];
            snprintf(fallback_name, sizeof(fallback_name), "Entry %lu (error)", i);
            menu_add_item(&disc_menu, fallback_name, MENU_ACTION_SELECT, NULL, NULL);
        }
    }

    disc_list_loaded = true;
    ESP_LOGI(TAG, "Directory list refreshed successfully");
    return ESP_OK;
}

// Reconnect to host after communication error
static void reconnect_to_host(void) {
    ESP_LOGI(TAG, "Reconnecting to host...");

    // Show reconnecting status on display
    ui_draw_info_screen(&display, "Reconnecting...", "Restoring communication\nwith main board...");
    display_manager_update(&display);

    disc_list_loaded = false;

    // Reset host async state
    host_comm_reset(&host_comm);

    // Re-enumerate directory
    esp_err_t ret = refresh_directory_list();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to re-enumerate after reconnect: %s", esp_err_to_name(ret));
        host_comm.initialized = false;
    }
}

// Handle communication error - attempt recovery
// Returns true if recovery succeeded
static bool handle_comm_error(esp_err_t err) {
    if (err == ESP_OK) {
        return true;
    }

    ESP_LOGW(TAG, "Communication error: %s, attempting recovery", esp_err_to_name(err));

    switch (err) {
        case ESP_ERR_TIMEOUT:
        case ESP_ERR_INVALID_RESPONSE:
        case ESP_FAIL:
            reconnect_to_host();
            return host_comm.initialized;
        default:
            return false;
    }
}

// Firmware update task
static void firmware_update_task(void *pvParameters) {
    ota_manager_t *ota = (ota_manager_t *)pvParameters;

    led_start_pulse(COLOR_RED);

    uint32_t last_display_update_ms = 0;
    uint8_t last_progress = 0;

    while (1) {
        esp_err_t ret = ota_manager_process(ota);
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FINISHED) {
            ESP_LOGE(TAG, "OTA process error: %s", esp_err_to_name(ret));
            break;
        }

        ota_state_t state = ota_manager_get_state(ota);
        uint8_t progress = ota_manager_get_progress(ota);

        // Update display only every 100ms or when progress changes
        uint32_t current_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (state != OTA_STATE_DOWNLOADING ||
            progress != last_progress ||
            (current_ms - last_display_update_ms) >= 100) {

            const char *status_msg = "Initializing...";
            switch (state) {
                case OTA_STATE_CHECKING:
                    status_msg = "Checking for updates...";
                    break;
                case OTA_STATE_DOWNLOADING:
                    status_msg = "Downloading firmware...";
                    break;
                case OTA_STATE_VERIFYING:
                    status_msg = "Verifying firmware...";
                    break;
                case OTA_STATE_APPLYING:
                    status_msg = "Applying update...";
                    break;
                case OTA_STATE_SUCCESS:
                    status_msg = "Update complete!";
                    break;
                case OTA_STATE_ERROR:
                    status_msg = "Update failed!";
                    break;
                default:
                    break;
            }

            if (state != OTA_STATE_IDLE) {
                ui_draw_firmware_update(&display, status_msg, progress);
            }

            last_display_update_ms = current_ms;
            last_progress = progress;
        }

        // Exit on completion or error
        if (state == OTA_STATE_SUCCESS) {
            ESP_LOGI(TAG, "Firmware update successful, restarting in 3 seconds...");
            led_stop_pulse();
            led_set_color(COLOR_GREEN);
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_restart();
        } else if (state == OTA_STATE_ERROR) {
            ESP_LOGE(TAG, "Firmware update failed");
            led_stop_pulse();
            led_set_color(COLOR_RED);
            vTaskDelay(pdMS_TO_TICKS(3000));
            break;
        }

        // Only yield briefly to other tasks, don't sleep
        taskYIELD();
    }

    led_stop_pulse();
    current_screen = SCREEN_MAIN_MENU;
    active_menu = screen_menus[current_screen];
    ui_draw_menu(&display, active_menu);
    vTaskDelete(NULL);
}

static void draw_firmware_status_screen(void) {
    uint32_t panel_current = ota_manager_get_current_version();
    ui_draw_firmware_status(&display,
                            panel_current, panel_fw_info.version, panel_fw_info.available,
                            rp2350_fw_status.current_version, rp2350_fw_status.available_version,
                            rp2350_fw_status.available_version != 0,
                            fw_screen_selection);
}

// Check firmware status for both panel and main board
static void check_firmware_status(void) {
    ESP_LOGI(TAG, "Checking firmware status...");

    // Show loading screen
    ui_draw_info_screen(&display, "Firmware", "Checking...");
    display_manager_update(&display);  // Force immediate display update before blocking operations

    fw_status_valid = false;
    fw_screen_selection = 0;
    memset(&panel_fw_info, 0, sizeof(panel_fw_info));
    memset(&rp2350_fw_status, 0, sizeof(rp2350_fw_status));

    if (!host_comm.initialized) {
        ESP_LOGW(TAG, "Host communication not initialized");
        ui_draw_info_screen(&display, "Error", "Host not connected");
        display_manager_update(&display);
        return;
    }

    // Initialize OTA manager if not already done
    esp_err_t ret = ota_manager_init(&ota_manager, &host_comm);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to initialize OTA manager: %s", esp_err_to_name(ret));
    }

    // Check for panel firmware update
    ret = host_comm_check_firmware(&host_comm, &panel_fw_info);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to check panel firmware: %s", esp_err_to_name(ret));
    }

    // Check for main board firmware update
    ret = host_comm_get_rp2350_fw_status(&host_comm, &rp2350_fw_status);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get RP2350 firmware status: %s", esp_err_to_name(ret));
    }

    fw_status_valid = true;
    draw_firmware_status_screen();
    display_manager_update(&display);
}

// Trigger panel firmware update
static void trigger_panel_update(void) {
    ESP_LOGI(TAG, "Triggering panel firmware update...");

    // Initialize OTA manager if needed
    esp_err_t ret = ota_manager_init(&ota_manager, &host_comm);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize OTA manager: %s", esp_err_to_name(ret));
        ui_draw_info_screen(&display, "Error", "Failed to init OTA");
        return;
    }

    // Copy firmware info to OTA manager
    memcpy(&ota_manager.firmware_info, &panel_fw_info, sizeof(panel_firmware_info_t));

    ret = ota_manager_start_update(&ota_manager);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start update: %s", esp_err_to_name(ret));
        ui_draw_info_screen(&display, "Error", "Failed to start update");
        return;
    }

    current_screen = SCREEN_FIRMWARE_UPDATE;

    char version_str[14];
    ota_manager_format_version_string(version_str, panel_fw_info.version);
    char status_msg[64];
    snprintf(status_msg, sizeof(status_msg), "Updating panel: v%s", version_str);
    ui_draw_firmware_update(&display, status_msg, 0);
    vTaskDelay(pdMS_TO_TICKS(1500));

    xTaskCreate(firmware_update_task, "firmware_update", 8192, &ota_manager, 10, NULL);
}

// Trigger main board firmware update
static void trigger_mainboard_update(void) {
    ESP_LOGI(TAG, "Triggering main board firmware update...");

    // Show updating screen
    ui_draw_firmware_update(&display, "Main board updating...", 0);

    esp_err_t ret = host_comm_start_rp2350_update(&host_comm);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start RP2350 update: %s", esp_err_to_name(ret));
        ui_draw_info_screen(&display, "Error", "Failed to start update");
        vTaskDelay(pdMS_TO_TICKS(2000));
        current_screen = SCREEN_SETTINGS;
        active_menu = screen_menus[current_screen];
        return;
    }

    // Show progress animation for 5 seconds while update happens
    // The actual update is fast, but we show progress for better UX
    for (int progress = 0; progress <= 100; progress += 2) {
        char msg[32];
        snprintf(msg, sizeof(msg), "Main board: %d%%", progress);
        ui_draw_firmware_update(&display, msg, progress);
        vTaskDelay(pdMS_TO_TICKS(100));  // 50 steps * 100ms = 5 seconds
    }

    // Now check if the board is back online
    ui_draw_info_screen(&display, "Main Board", "Waiting for reboot...");

    int attempts = 0;
    while (attempts < 20) {  // Up to 10 seconds
        vTaskDelay(pdMS_TO_TICKS(500));

        rp2350_fw_status_t fw_status;
        ret = host_comm_get_rp2350_fw_status(&host_comm, &fw_status);

        if (ret == ESP_OK) {
            // Board is back! Show new version
            char version_str[16];
            uint8_t major = (fw_status.current_version >> 16) & 0xFF;
            uint8_t minor = (fw_status.current_version >> 8) & 0xFF;
            uint8_t patch = fw_status.current_version & 0xFF;
            snprintf(version_str, sizeof(version_str), "%d.%d.%d", major, minor, patch);

            char msg[48];
            snprintf(msg, sizeof(msg), "Update complete!\nNow running v%s", version_str);
            ui_draw_info_screen(&display, "Main Board", msg);
            vTaskDelay(pdMS_TO_TICKS(3000));
            break;
        }

        attempts++;
    }

    if (attempts >= 20) {
        ui_draw_info_screen(&display, "Main Board", "Update sent.\nBoard may still be\nrebooting...");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    // Return to settings
    current_screen = SCREEN_SETTINGS;
    active_menu = screen_menus[current_screen];
}

// Host communication task (currently unused)
#if 0
static void host_comm_task(void *pvParameters) {
    // Wait a bit for system to stabilize
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    while (1) {
        if (host_comm.initialized) {
            // Periodically check host status
            host_status_t status;
            esp_err_t ret = host_comm_get_status(&host_comm, &status);
            if (ret == ESP_OK) {
                ESP_LOGD(TAG, "Host status: 0x%02X", status);
            } else {
                ESP_LOGW(TAG, "Failed to get host status: %s", esp_err_to_name(ret));
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
#endif

void app_main(void) {
    ESP_LOGI(TAG, "%s Starting...", PRODUCT_NAME_FULL);

    // Initialize shared SPI bus with MISO enabled for host communication
    spi_bus_config_t bus_config = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_CLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = 4096,
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize shared SPI bus: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Shared SPI bus initialized (MISO: %d, MOSI: %d, CLK: %d)",
             PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SPI_CLK);

    ret = display_manager_init(&display);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display: %s", esp_err_to_name(ret));
        return;
    }

    xTaskCreate(display_update_task, "display_update", 2048, NULL, 5, NULL);
    xTaskCreate(status_refresh_task, "status_refresh", 2048, NULL, 4, NULL);

    ui_show_splash_screen(&display);
    const esp_app_desc_t* app_desc = esp_app_get_description();
    ui_update_splash_versions(&display, app_desc->version, NULL);
    ui_update_splash_progress(&display, "Init display...", 10);

    ui_update_splash_progress(&display, "Init LED...", 20);
    ret = led_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LED driver: %s", esp_err_to_name(ret));
        return;
    }
    led_start_pulse(COLOR_CYAN);

    ui_update_splash_progress(&display, "Init GPIO...", 30);
    ret = gpio_handler_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize gpio handler: %s", esp_err_to_name(ret));
        return;
    }

    ret = gpio_handler_configure_activity_pin(PIN_ACT_IN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure activity pin: %s", esp_err_to_name(ret));
        return;
    }

    ret = gpio_handler_install_activity_isr(PIN_ACT_IN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install activity ISR: %s", esp_err_to_name(ret));
        return;
    }

    // Register custom activity handler for LED control
    gpio_handler_register_activity_callback(handle_activity_event);

    // Set initial LED to orange (no image loaded yet)
    led_set_color(COLOR_ORANGE);
    
    ui_update_splash_progress(&display, "Init buttons...", 40);
    // Configure buttons with repeat for up/down navigation
    button_config_t nav_button_config = {
        .active_low = true,
        .enable_double_click = false,
        .enable_long_press = true,
        .enable_repeat = true,
        .debounce_ms = 100,
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
        .debounce_ms = 100,
        .double_click_ms = 300,
        .long_press_ms = 1000,
        .repeat_ms = 100
    };
    
    nav_button_config.gpio = PIN_NAV_UP;
    gpio_handler_add_button(0, &nav_button_config);

    nav_button_config.gpio = PIN_NAV_DOWN;
    gpio_handler_add_button(2, &nav_button_config);

    action_button_config.gpio = PIN_NAV_RIGHT;
    gpio_handler_add_button(1, &action_button_config);

    action_button_config.gpio = PIN_NAV_LEFT;
    gpio_handler_add_button(3, &action_button_config);

    gpio_handler_register_button_callback(handle_button_event);
    gpio_handler_start();

    ui_update_splash_progress(&display, "Init menus...", 50);
    menu_init(&main_menu, MENU_VISIBLE_ITEMS_WITH_TITLE);
    main_menu.title = "Main Menu";
    menu_set_items(&main_menu, main_menu_items,
                  sizeof(main_menu_items) / sizeof(main_menu_items[0]));

    menu_init(&disc_menu, MENU_VISIBLE_ITEMS_WITH_TITLE);
    disc_menu.title = "Select Image";

    menu_init(&settings_menu, MENU_VISIBLE_ITEMS_WITH_TITLE);
    settings_menu.title = "Settings";
    menu_set_items(&settings_menu, settings_menu_items,
                  sizeof(settings_menu_items) / sizeof(settings_menu_items[0]));

    menu_init(&wifi_menu, MENU_VISIBLE_ITEMS_WITH_TITLE);
    wifi_menu.title = "WiFi Setup";
    menu_set_items(&wifi_menu, wifi_menu_items,
                  sizeof(wifi_menu_items) / sizeof(wifi_menu_items[0]));

    transport_config_t transport_cfg = {
        .device_addr = HOST_DEVICE_ADDR,
        .sda_miso = PIN_SDA,
        .scl_clk = PIN_SCL,
        .cs = PIN_HOST_CS,
        .mosi = PIN_SPI_MOSI,
        .clock_speed = HOST_CLOCK_SPEED,
        .timeout_ms = HOST_TIMEOUT_MS,
    };

    ui_update_splash_progress(&display, "Establish comms...", 55);
    vTaskDelay(pdMS_TO_TICKS(1000));
    ret = host_comm_init(&host_comm, &transport_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize host comm: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Host comm initialized successfully using %s",
                 host_comm_get_transport_name(&host_comm));

        uint8_t test_status;
        ret = host_comm_get_device_status(&host_comm, &test_status);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to communicate with main board: %s", esp_err_to_name(ret));
            led_stop_pulse();

            for (int i = 0; i < 6; i++) {
                led_set_color(COLOR_RED);
                vTaskDelay(pdMS_TO_TICKS(200));
                led_clear();
                vTaskDelay(pdMS_TO_TICKS(200));
            }

            display_manager_clear(&display);
            display_manager_set_font(&display, u8g2_font_amstrad_cpc_extended_8f);
            display_manager_draw_text(&display, 10, 16, "Communication");
            display_manager_draw_text(&display, 30, 32, "Error!");
            display_manager_set_font(&display, u8g2_font_6x10_tf);
            display_manager_draw_text(&display, 8, 48, "Cannot reach");
            display_manager_draw_text(&display, 8, 58, "main board");
            display_manager_request_update(&display);
            display_manager_update(&display);

            vTaskDelay(pdMS_TO_TICKS(3000));
            led_start_pulse(COLOR_CYAN);
            host_comm.initialized = false;
        } else {
            ESP_LOGI(TAG, "Communication with main board verified (status: 0x%02X)", test_status);

            // Get and display main board firmware version
            rp2350_fw_status_t fw_status;
            if (host_comm_get_rp2350_fw_status(&host_comm, &fw_status) == ESP_OK) {
                char main_version[16];
                snprintf(main_version, sizeof(main_version), "%lu.%lu.%lu",
                         (fw_status.current_version >> 16) & 0xFF,
                         (fw_status.current_version >> 8) & 0xFF,
                         fw_status.current_version & 0xFF);
                ui_update_splash_versions(&display, app_desc->version, main_version);
                ESP_LOGI(TAG, "Main board firmware version: %s", main_version);
            }

            // Get device type (IDE vs ATAPI)
            refresh_device_type();

            ESP_LOGI(TAG, "Fetching directory list at startup...");
            ui_update_splash_progress(&display, "Load directory...", 70);
            esp_err_t disc_ret = refresh_directory_list();
            if (disc_ret == ESP_OK) {
                ESP_LOGI(TAG, "Directory list loaded successfully at startup");
            } else {
                ESP_LOGW(TAG, "Failed to load directory list at startup: %s", esp_err_to_name(disc_ret));
            }
        }
    }

    ui_update_splash_progress(&display, "Init WiFi...", 80);
    ret = wifi_manager_init(&wifi_manager);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize WiFi manager: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "WiFi manager initialized successfully");

        wifi_manager_config_t wifi_config;
        ret = wifi_manager_get_config(&wifi_manager, &wifi_config);
        if (ret == ESP_OK && wifi_config.auto_connect && strlen(wifi_config.ssid) > 0) {
            ESP_LOGI(TAG, "Auto-connecting to saved WiFi: %s", wifi_config.ssid);
            wifi_manager_connect(&wifi_manager, wifi_config.ssid, wifi_config.password);
        } else {
            ESP_LOGI(TAG, "Starting WiFi AP mode for configuration");
            wifi_manager_start_ap(&wifi_manager);
        }
    }

    ret = web_server_init(&web_server, WEB_SERVER_PORT);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize web server: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Web server initialized successfully");
    }

    interface_ctx.host_comm = &host_comm;
    interface_ctx.wifi_manager = &wifi_manager;

    web_server_set_interface_context(&web_server, &interface_ctx);

    ui_update_splash_progress(&display, "Start services...", 90);
    ESP_LOGI(TAG, "Starting web server");
    ret = web_server_start(&web_server);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start web server: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Web server started successfully");
        ESP_LOGI(TAG, "Access web interface at: http://%s.local (when WiFi connects)", WIFI_MANAGER_MDNS_HOSTNAME);
    }

    ui_update_splash_progress(&display, "Ready!", 100);
    vTaskDelay(pdMS_TO_TICKS(500));

    led_stop_pulse();

    current_screen = SCREEN_STATUS;
    active_menu = screen_menus[current_screen];
    refresh_playback_status();
    ui_draw_status_screen(&display, current_disc_name,
                          &current_image_status,
                          &current_playback_status, disc_name_changed);
    disc_name_changed = false;
    status_needs_redraw = false;

    // Set LED based on image loaded state
    activity_event_t init_event = { .pin_state = gpio_get_level(PIN_ACT_IN) };
    handle_activity_event(&init_event);

    ESP_LOGI(TAG, "System initialized successfully");

    // If we booted from a new OTA firmware, mark it as valid now that init succeeded
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "New firmware verified successfully, cancelling rollback");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    // Main loop can be used for other tasks or left empty
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
