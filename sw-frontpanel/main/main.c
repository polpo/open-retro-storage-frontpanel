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
#include "nvs_flash.h"

static const char *TAG = "main";

// Global instances
static display_manager_t display;
static menu_t main_menu;
static menu_t disc_menu;
static menu_t wifi_menu;
static menu_t settings_menu;
static menu_t display_settings_menu;  // Parent: Idle Mode / Idle Timeout / Blank Timeout
static menu_t idle_mode_menu;
static menu_t idle_timeout_menu;
static menu_t blank_timeout_menu;
static menu_t *active_menu = NULL;  // Pointer to currently displayed menu
static screen_type_t info_return_screen = SCREEN_MAIN_MENU;  // Screen to return to from SCREEN_INFO

// Menu lookup by screen type (screens without menus are NULL)
static menu_t* screen_menus[SCREEN_COUNT] = {
    [SCREEN_MAIN_MENU] = &main_menu,
    [SCREEN_DISC_LIST] = &disc_menu,
    [SCREEN_SETTINGS] = &settings_menu,
    [SCREEN_WIFI_MENU] = &wifi_menu,
    [SCREEN_DISPLAY_SETTINGS] = &display_settings_menu,
    [SCREEN_IDLE_MODE] = &idle_mode_menu,
    [SCREEN_IDLE_TIMEOUT] = &idle_timeout_menu,
    [SCREEN_BLANK_TIMEOUT] = &blank_timeout_menu,
};
static host_comm_t host_comm;
static wifi_manager_t wifi_manager;
static web_server_t web_server;
static interface_context_t interface_ctx;
static screen_type_t current_screen = SCREEN_SPLASH;
static ota_manager_t ota_manager;

// Dynamic disc list - populated from I2C communication
static bool disc_list_loaded = false;
static char current_disc_name[64] = "No main board";
static playback_status_t current_playback_status = {0};
static bool disc_name_changed = true;  // Flag to signal title changed (resets scroll)
static bool status_needs_redraw = true;  // Flag to signal status screen needs redraw
static loaded_image_status_t current_image_status = {0};  // Full image status including directory

// Device type (IDE vs ATAPI) - affects available operations
static uint8_t current_device_type = PANEL_DEVICE_TYPE_ATAPI;  // Default to ATAPI

// Latest PANEL_CMD_GET_DEVICE_STATUS reply, refreshed alongside playback
// status. Drives the LED color so SD error states (NO_CARD, WRONG_MODE) get
// distinct indication beyond just "no disc."
static uint8_t current_device_status = PANEL_DEVICE_STATUS_NO_IMAGE;

// Firmware status tracking
static rp2350_fw_status_t rp2350_fw_status = {0};
static panel_firmware_info_t panel_fw_info = {0};
static bool fw_status_valid = false;

// Forward declarations
static esp_err_t refresh_directory_list(void);
static void refresh_playback_status(void);
static void refresh_device_type(void);
static void check_firmware_status(void);
static void draw_firmware_status_screen(void);
static void trigger_system_update(void);
static void show_info_screen(const char *title, const char *body);
static void redraw_current_screen(void);

// SCREEN_INFO state. Title is always a string literal (caller-owned). Body is
// often built on the caller's stack, so we copy it so the screen survives
// being re-rendered later (e.g. after the screensaver clears the buffer).
static const char *info_screen_title = "";
static char info_screen_body[256] = "";

// Cached state used to refresh the LED both on activity events and on display
// off-state transitions so breathing can pick up the current color
static activity_event_t last_activity_event = {0};
static bool display_is_off = false;

// Computes the LED color implied by current state. Returns true via the
// out-param when the color represents in-progress disc activity, which
// suppresses the slow breathe so reads/writes remain clearly visible
static rgb_color_t compute_led_state_color(bool *is_activity) {
    *is_activity = false;
    // SD-card error states win over the normal disc-inserted check so the
    // user sees a clearly different color than the generic "no image" idle
    switch (current_device_status) {
        case PANEL_DEVICE_STATUS_NO_CARD:
            return COLOR_RED;
        case PANEL_DEVICE_STATUS_WRONG_MODE:
            return COLOR_MAGENTA;
        default:
            break;
    }
    if (!current_playback_status.disc_inserted) {
        return COLOR_ORANGE;  // No image loaded
    }
    if (last_activity_event.pin_state) {
        *is_activity = true;
        return COLOR_YELLOW;  // Activity
    }
    return COLOR_CYAN;        // Idle with image
}

// Re-applies the LED color and breathe state from current cached state.
// Safe to call whenever device status, playback status, last activity event,
// or display-off state changes
static void refresh_led_state(void) {
    bool is_activity;
    rgb_color_t color = compute_led_state_color(&is_activity);

    if (display_is_off && !is_activity) {
        led_set_color(color);     // updates cache; breathe task picks up
        led_start_breathe();      // idempotent
    } else {
        led_stop_breathe();       // idempotent
        led_set_color(color);
    }
}

// Activity LED handler
static void handle_activity_event(activity_event_t *event) {
    if (!event) return;
    last_activity_event = *event;
    refresh_led_state();
}

static void on_display_off_state_change(bool now_off, void *ctx) {
    (void)ctx;
    display_is_off = now_off;
    refresh_led_state();
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

// Display Settings parent — text rewritten by refresh_display_settings_labels()
// to show the currently-selected value of each sub-setting.
static menu_item_t display_settings_menu_items[] = {
    {.text = "Idle Mode", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "Idle Timeout", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "Blank Timeout", .action = MENU_ACTION_CUSTOM, .selectable = true},
};

// Idle Mode picker — order matches display_idle_mode_t enum values.
// To disable the idle stage entirely, set Idle Timeout to "Never".
static menu_item_t idle_mode_menu_items[] = {
    {.text = "Dim", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "Flying Toasters", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "DVD Logo", .action = MENU_ACTION_CUSTOM, .selectable = true},
};
static const char *idle_mode_names[] = { "Dim", "Flying Toasters", "DVD Logo" };

// Idle Timeout picker. Values in milliseconds; labels rebuilt with selection mark.
// DISPLAY_IDLE_TIMEOUT_NEVER (0) disables the dim/screensaver intermediate stage.
static const uint32_t idle_timeout_options_ms[] = {
    30 * 1000,
    60 * 1000,
    120 * 1000,
    300 * 1000,
    DISPLAY_IDLE_TIMEOUT_NEVER,
};
static const char *idle_timeout_labels[] = { "30 sec", "1 min", "2 min", "5 min", "Never" };
#define IDLE_TIMEOUT_OPTION_COUNT (sizeof(idle_timeout_options_ms) / sizeof(idle_timeout_options_ms[0]))
static menu_item_t idle_timeout_menu_items[IDLE_TIMEOUT_OPTION_COUNT] = {
    {.action = MENU_ACTION_CUSTOM, .selectable = true},
    {.action = MENU_ACTION_CUSTOM, .selectable = true},
    {.action = MENU_ACTION_CUSTOM, .selectable = true},
    {.action = MENU_ACTION_CUSTOM, .selectable = true},
    {.action = MENU_ACTION_CUSTOM, .selectable = true},
};

// Blank Timeout picker. Total inactivity time before the screen is fully blanked.
// While blanked, the LED slowly breathes the current idle color.
// DISPLAY_OFF_TIMEOUT_NEVER (0) disables blanking entirely.
static const uint32_t blank_timeout_options_ms[] = {
    10 * 60 * 1000,
    20 * 60 * 1000,
    30 * 60 * 1000,
    60 * 60 * 1000,
    DISPLAY_OFF_TIMEOUT_NEVER,
};
static const char *blank_timeout_labels[] = { "10 min", "20 min", "30 min", "1 hour", "Never" };
#define BLANK_TIMEOUT_OPTION_COUNT (sizeof(blank_timeout_options_ms) / sizeof(blank_timeout_options_ms[0]))
static menu_item_t blank_timeout_menu_items[BLANK_TIMEOUT_OPTION_COUNT] = {
    {.action = MENU_ACTION_CUSTOM, .selectable = true},
    {.action = MENU_ACTION_CUSTOM, .selectable = true},
    {.action = MENU_ACTION_CUSTOM, .selectable = true},
    {.action = MENU_ACTION_CUSTOM, .selectable = true},
    {.action = MENU_ACTION_CUSTOM, .selectable = true},
};

static const char *idle_timeout_short_label(uint32_t timeout_ms) {
    for (size_t i = 0; i < IDLE_TIMEOUT_OPTION_COUNT; i++) {
        if (idle_timeout_options_ms[i] == timeout_ms) return idle_timeout_labels[i];
    }
    return "Custom";
}

static const char *blank_timeout_short_label(uint32_t timeout_ms) {
    for (size_t i = 0; i < BLANK_TIMEOUT_OPTION_COUNT; i++) {
        if (blank_timeout_options_ms[i] == timeout_ms) return blank_timeout_labels[i];
    }
    return "Custom";
}

static void refresh_idle_mode_labels(void) {
    display_idle_mode_t current = display_manager_get_idle_mode(&display);
    for (size_t i = 0; i < sizeof(idle_mode_names) / sizeof(idle_mode_names[0]); i++) {
        snprintf(idle_mode_menu_items[i].text, MENU_ITEM_MAX_LENGTH,
                 "%s %s", (current == (display_idle_mode_t)i) ? "*" : " ", idle_mode_names[i]);
    }
    idle_mode_menu.needs_redraw = true;
}

static void refresh_idle_timeout_labels(void) {
    uint32_t current = display_manager_get_idle_timeout(&display);
    for (size_t i = 0; i < IDLE_TIMEOUT_OPTION_COUNT; i++) {
        snprintf(idle_timeout_menu_items[i].text, MENU_ITEM_MAX_LENGTH,
                 "%s %s", (idle_timeout_options_ms[i] == current) ? "*" : " ",
                 idle_timeout_labels[i]);
    }
    idle_timeout_menu.needs_redraw = true;
}

static void refresh_blank_timeout_labels(void) {
    uint32_t current = display_manager_get_off_timeout(&display);
    for (size_t i = 0; i < BLANK_TIMEOUT_OPTION_COUNT; i++) {
        snprintf(blank_timeout_menu_items[i].text, MENU_ITEM_MAX_LENGTH,
                 "%s %s", (blank_timeout_options_ms[i] == current) ? "*" : " ",
                 blank_timeout_labels[i]);
    }
    blank_timeout_menu.needs_redraw = true;
}

static void refresh_display_settings_labels(void) {
    snprintf(display_settings_menu_items[0].text, MENU_ITEM_MAX_LENGTH,
             "Idle Mode: %s", idle_mode_names[display_manager_get_idle_mode(&display)]);
    snprintf(display_settings_menu_items[1].text, MENU_ITEM_MAX_LENGTH,
             "Timeout: %s", idle_timeout_short_label(display_manager_get_idle_timeout(&display)));
    snprintf(display_settings_menu_items[2].text, MENU_ITEM_MAX_LENGTH,
             "Blank: %s", blank_timeout_short_label(display_manager_get_off_timeout(&display)));
    display_settings_menu.needs_redraw = true;
}

// Button event handler
static void handle_button_event(button_event_t *event) {
    if (!event) return;

    ESP_LOGI(TAG, "Button %d event: %d", event->button_id, event->type);

    // Only CLICK events wake display and reset activity timer
    // This avoids double-processing since PRESS fires before CLICK
    if (event->type == BUTTON_EVENT_CLICK) {
        esp_err_t wake_result = display_manager_wake(&display);
        if (wake_result == ESP_ERR_NOT_FINISHED) {
            ESP_LOGI(TAG, "Display was idle - just waking, ignoring action");
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
                                show_info_screen("Not Available",
                                    "Eject is not available\nin IDE mode.\n\nUse Select Image to\nchoose a different\nhard disk image.");
                            } else if (host_comm.initialized) {
                                esp_err_t ret = host_comm_eject_image(&host_comm);
                                if (ret == ESP_OK) {
                                    ESP_LOGI(TAG, "Image ejected");
                                    current_screen = SCREEN_STATUS;
                                    refresh_playback_status();
                                } else {
                                    ESP_LOGW(TAG, "Failed to eject disc: %s", esp_err_to_name(ret));
                                }
                            }
                        } else if (selected == 2) { // "Settings"
                            current_screen = SCREEN_SETTINGS;
                        } else if (selected == 3) { // "System Info"
                            info_return_screen = SCREEN_MAIN_MENU;
                            current_screen = SCREEN_INFO;
                            const esp_app_desc_t* info_app_desc = esp_app_get_description();
                            char main_ver_str[20] = "N/A";
                            if (host_comm.initialized) {
                                rp2350_fw_status_t fw_status;
                                if (host_comm_get_rp2350_fw_status(&host_comm, &fw_status) == ESP_OK) {
                                    ota_manager_format_version_string(main_ver_str, fw_status.current_version);
                                }
                            }
                            char info_text[160];
                            snprintf(info_text, sizeof(info_text),
                                     "PicoIDE Front Panel\nPanel: v%s\nMain:  v%s\n%s: %s",
                                     info_app_desc->version,
                                     main_ver_str,
                                     host_comm_get_transport_name(&host_comm),
                                     host_comm.initialized ? "Connected" : "Disconnected");
                            show_info_screen("System Info", info_text);
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
                                        show_info_screen("Image Selected",
                                            "Image will be loaded\non next power cycle.\n\nPress back to\nreturn to browser.");
                                    } else {
                                        // ATAPI mode: image loaded, return to status screen
                                        current_screen = SCREEN_STATUS;
                                        refresh_playback_status();
                                    }
                                } else {
                                    ESP_LOGW(TAG, "Failed to select entry via host comm: %s", esp_err_to_name(ret));
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
                            refresh_display_settings_labels();
                            current_screen = SCREEN_DISPLAY_SETTINGS;
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
                            show_info_screen("WiFi Status", info_text);
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
                            show_info_screen("WiFi Reset", info_text);
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

        case SCREEN_DISPLAY_SETTINGS:
            switch (event->button_id) {
                case 0: // Up (North)
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_up(&display_settings_menu);
                    }
                    break;
                case 2: // Down (South)
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_down(&display_settings_menu);
                    }
                    break;
                case 1: // Select (East)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        uint32_t selected = menu_get_selected_index(&display_settings_menu);
                        if (selected == 0) {
                            refresh_idle_mode_labels();
                            current_screen = SCREEN_IDLE_MODE;
                        } else if (selected == 1) {
                            refresh_idle_timeout_labels();
                            current_screen = SCREEN_IDLE_TIMEOUT;
                        } else if (selected == 2) {
                            refresh_blank_timeout_labels();
                            current_screen = SCREEN_BLANK_TIMEOUT;
                        }
                    }
                    break;
                case 3: // Back (West)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        current_screen = SCREEN_SETTINGS;
                    }
                    break;
            }
            break;

        case SCREEN_IDLE_MODE:
            switch (event->button_id) {
                case 0:
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_up(&idle_mode_menu);
                    }
                    break;
                case 2:
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_down(&idle_mode_menu);
                    }
                    break;
                case 1:
                    if (event->type == BUTTON_EVENT_CLICK) {
                        uint32_t selected = menu_get_selected_index(&idle_mode_menu);
                        display_manager_set_idle_mode(&display, (display_idle_mode_t)selected);
                        refresh_idle_mode_labels();
                    }
                    break;
                case 3:
                    if (event->type == BUTTON_EVENT_CLICK) {
                        refresh_display_settings_labels();
                        current_screen = SCREEN_DISPLAY_SETTINGS;
                    }
                    break;
            }
            break;

        case SCREEN_IDLE_TIMEOUT:
            switch (event->button_id) {
                case 0:
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_up(&idle_timeout_menu);
                    }
                    break;
                case 2:
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_down(&idle_timeout_menu);
                    }
                    break;
                case 1:
                    if (event->type == BUTTON_EVENT_CLICK) {
                        uint32_t selected = menu_get_selected_index(&idle_timeout_menu);
                        if (selected < IDLE_TIMEOUT_OPTION_COUNT) {
                            display_manager_set_idle_timeout(&display,
                                                             idle_timeout_options_ms[selected]);
                            refresh_idle_timeout_labels();
                        }
                    }
                    break;
                case 3:
                    if (event->type == BUTTON_EVENT_CLICK) {
                        refresh_display_settings_labels();
                        current_screen = SCREEN_DISPLAY_SETTINGS;
                    }
                    break;
            }
            break;

        case SCREEN_BLANK_TIMEOUT:
            switch (event->button_id) {
                case 0:
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_up(&blank_timeout_menu);
                    }
                    break;
                case 2:
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_down(&blank_timeout_menu);
                    }
                    break;
                case 1:
                    if (event->type == BUTTON_EVENT_CLICK) {
                        uint32_t selected = menu_get_selected_index(&blank_timeout_menu);
                        if (selected < BLANK_TIMEOUT_OPTION_COUNT) {
                            display_manager_set_off_timeout(&display,
                                                            blank_timeout_options_ms[selected]);
                            refresh_blank_timeout_labels();
                        }
                    }
                    break;
                case 3:
                    if (event->type == BUTTON_EVENT_CLICK) {
                        refresh_display_settings_labels();
                        current_screen = SCREEN_DISPLAY_SETTINGS;
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
                case 1: // Select button (East) - trigger unified update
                    if (event->type == BUTTON_EVENT_CLICK) {
                        bool panel_needs_update = panel_fw_info.available &&
                            (panel_fw_info.version > ota_manager_get_current_version());
                        bool main_needs_update = (rp2350_fw_status.available_version != 0);
                        if (panel_needs_update || main_needs_update) {
                            trigger_system_update();
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

static void show_info_screen(const char *title, const char *body) {
    info_screen_title = title ? title : "";
    if (body) {
        strncpy(info_screen_body, body, sizeof(info_screen_body) - 1);
        info_screen_body[sizeof(info_screen_body) - 1] = '\0';
    } else {
        info_screen_body[0] = '\0';
    }
    ui_draw_info_screen(&display, info_screen_title, info_screen_body);
}

// Re-render whatever screen is currently active. Used after the screensaver
// has overwritten the buffer.
static void redraw_current_screen(void) {
    switch (current_screen) {
        case SCREEN_STATUS:
            disc_name_changed = true;     // Reset scroll animation
            status_needs_redraw = true;   // display_update_task will draw next tick
            break;
        case SCREEN_MAIN_MENU:
        case SCREEN_DISC_LIST:
        case SCREEN_SETTINGS:
        case SCREEN_WIFI_MENU:
        case SCREEN_DISPLAY_SETTINGS:
        case SCREEN_IDLE_MODE:
        case SCREEN_IDLE_TIMEOUT:
        case SCREEN_BLANK_TIMEOUT:
            if (active_menu) active_menu->needs_redraw = true;
            break;
        case SCREEN_INFO:
            ui_draw_info_screen(&display, info_screen_title, info_screen_body);
            break;
        case SCREEN_FIRMWARE_STATUS:
            draw_firmware_status_screen();
            break;
        case SCREEN_FIRMWARE_UPDATE:
        case SCREEN_SPLASH:
        default:
            // FW update task owns its own rendering; splash only runs at boot
            break;
    }
}

// Display update task
static void display_update_task(void *pvParameters) {
    static screen_type_t last_screen = SCREEN_COUNT;  // Invalid value forces initial draw

    while (1) {
        if (display.initialized) {
            // Honor a deferred full-redraw request first (e.g. waking from screensaver).
            if (display.needs_full_redraw) {
                display.needs_full_redraw = false;
                redraw_current_screen();
            }

            bool screen_changed = (current_screen != last_screen);
            last_screen = current_screen;

            if (display_manager_screensaver_active(&display)) {
                display_manager_screensaver_tick(&display);
            } else if (current_screen == SCREEN_STATUS) {
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

    // Liveness check via the device status read
    uint8_t status_byte;
    if (!host_comm.initialized ||
        host_comm_get_device_status(&host_comm, &status_byte) != ESP_OK) {
        memset(&current_playback_status, 0, sizeof(current_playback_status));
        memset(&current_image_status, 0, sizeof(current_image_status));
        strncpy(current_disc_name, "No main board", sizeof(current_disc_name) - 1);
        current_disc_name[sizeof(current_disc_name) - 1] = '\0';
        // This path returns early, so flag the redraw here the way the
        // normal path does at the end
        if (strcmp(old_disc_name, current_disc_name) != 0) {
            disc_name_changed = true;  // Refresh the status title, reset scroll
        }
        status_needs_redraw = true;
        return;
    }

    esp_err_t ret = host_comm_get_playback_status(&host_comm, &current_playback_status);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get playback status: %s", esp_err_to_name(ret));
        memset(&current_playback_status, 0, sizeof(current_playback_status));
        strncpy(current_disc_name, "No main board", sizeof(current_disc_name) - 1);
        current_disc_name[sizeof(current_disc_name) - 1] = '\0';
    } else if (current_playback_status.disc_name[0] != '\0') {
      // Trust whatever string the main board put in disc_name — it carries the
      // image name when a disc is loaded, and the error text ("No SD card" /
      // "Wrong-mode card") when in an SD error state
      strncpy(current_disc_name, current_playback_status.disc_name,
              sizeof(current_disc_name) - 1);
      current_disc_name[sizeof(current_disc_name) - 1] = '\0';
    } else {
        strncpy(current_disc_name, "No image loaded", sizeof(current_disc_name) - 1);
        current_disc_name[sizeof(current_disc_name) - 1] = '\0';
    }

    // Track recovery from an SD error so a stale directory listing from the old
    // card gets refetched
    uint8_t prev = current_device_status;
    current_device_status = status_byte;
    bool prev_err = (prev == PANEL_DEVICE_STATUS_NO_CARD ||
                     prev == PANEL_DEVICE_STATUS_WRONG_MODE);
    bool now_err  = (status_byte == PANEL_DEVICE_STATUS_NO_CARD ||
                     status_byte == PANEL_DEVICE_STATUS_WRONG_MODE);
    if (prev_err && !now_err) {
        ESP_LOGI(TAG, "SD card recovered; invalidating disc list cache");
        disc_list_loaded = false;
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

// Firmware update task
// Unified firmware update task: panel OTA (if needed) -> RP2350 update -> wait -> reboot self
static void firmware_update_task(void *pvParameters) {
    led_start_pulse(COLOR_RED);

    bool panel_needs_update = panel_fw_info.available &&
        (panel_fw_info.version > ota_manager_get_current_version());
    bool main_needs_update = (rp2350_fw_status.available_version != 0);
    bool panel_ota_written = false;

    // Phase 1: Panel OTA (download and write, but don't reboot yet)
    if (panel_needs_update) {
        ESP_LOGI(TAG, "Phase 1: Updating panel firmware...");
        ui_draw_firmware_update(&display, "Updating panel...", 0);

        // Initialize OTA manager
        esp_err_t ret = ota_manager_init(&ota_manager, &host_comm);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to initialize OTA manager: %s", esp_err_to_name(ret));
            goto error;
        }

        // Copy firmware info and start OTA
        memcpy(&ota_manager.firmware_info, &panel_fw_info, sizeof(panel_firmware_info_t));
        ret = ota_manager_start_update(&ota_manager);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start panel OTA: %s", esp_err_to_name(ret));
            goto error;
        }

        // Process OTA chunks until complete
        uint8_t last_progress = 0;
        while (1) {
            ret = ota_manager_process(&ota_manager);
            if (ret != ESP_OK && ret != ESP_ERR_NOT_FINISHED) {
                ESP_LOGE(TAG, "Panel OTA error: %s", esp_err_to_name(ret));
                goto error;
            }

            ota_state_t state = ota_manager_get_state(&ota_manager);
            uint8_t progress = ota_manager_get_progress(&ota_manager);

            if (progress != last_progress) {
                ui_draw_firmware_update(&display, "Updating panel...", progress);
                last_progress = progress;
            }

            if (state == OTA_STATE_SUCCESS) {
                panel_ota_written = true;
                ESP_LOGI(TAG, "Panel OTA written successfully (reboot deferred)");
                break;
            } else if (state == OTA_STATE_ERROR) {
                ESP_LOGE(TAG, "Panel OTA failed");
                goto error;
            }

            taskYIELD();
        }
    }

    // Phase 2: Main board update
    if (main_needs_update) {
        ESP_LOGI(TAG, "Phase 2: Updating main board...");
        ui_draw_firmware_update(&display, "Updating main board...", 0);

        esp_err_t ret = host_comm_start_rp2350_update(&host_comm);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start RP2350 update: %s", esp_err_to_name(ret));
            // If panel OTA was written, we have a version mismatch - still reboot panel
            if (panel_ota_written) {
                ui_draw_firmware_update(&display, "Main board failed!", 0);
                vTaskDelay(pdMS_TO_TICKS(2000));
                ESP_LOGW(TAG, "Rebooting panel despite main board failure");
                esp_restart();
            }
            goto error;
        }

        // Show progress animation while main board flashes and reboots
        for (int progress = 0; progress <= 100; progress += 2) {
            char msg[32];
            snprintf(msg, sizeof(msg), "Main board: %d%%", progress);
            ui_draw_firmware_update(&display, msg, progress);
            vTaskDelay(pdMS_TO_TICKS(100));  // 50 steps * 100ms = 5 seconds
        }

        // Wait for main board to come back online
        ui_draw_firmware_update(&display, "Waiting for reboot...", 100);

        bool main_board_back = false;
        for (int attempts = 0; attempts < 20; attempts++) {  // Up to 10 seconds
            vTaskDelay(pdMS_TO_TICKS(500));

            rp2350_fw_status_t fw_status;
            ret = host_comm_get_rp2350_fw_status(&host_comm, &fw_status);
            if (ret == ESP_OK) {
                char version_str[20];
                ota_manager_format_version_string(version_str, fw_status.current_version);
                ESP_LOGI(TAG, "Main board back online: v%s", version_str);
                main_board_back = true;
                break;
            }
        }

        if (!main_board_back) {
            ESP_LOGW(TAG, "Main board did not come back in time");
        }
    }

    // Phase 3: Reboot panel if OTA was written, otherwise just show success
    if (panel_ota_written) {
        ESP_LOGI(TAG, "Phase 3: Rebooting panel into new firmware...");
        ui_draw_firmware_update(&display, "Restarting...", 100);
        led_stop_pulse();
        led_set_color(COLOR_GREEN);
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }

    // Only main board was updated, no panel reboot needed
    ESP_LOGI(TAG, "System update complete");
    ui_draw_firmware_update(&display, "Update complete!", 100);
    led_stop_pulse();
    led_set_color(COLOR_GREEN);
    vTaskDelay(pdMS_TO_TICKS(3000));

    current_screen = SCREEN_SETTINGS;
    active_menu = screen_menus[current_screen];
    ui_draw_menu(&display, active_menu);
    vTaskDelete(NULL);
    return;

error:
    ui_draw_firmware_update(&display, "Update failed!", 0);
    led_stop_pulse();
    led_set_color(COLOR_RED);
    vTaskDelay(pdMS_TO_TICKS(3000));
    current_screen = SCREEN_SETTINGS;
    active_menu = screen_menus[current_screen];
    ui_draw_menu(&display, active_menu);
    vTaskDelete(NULL);
}

static void draw_firmware_status_screen(void) {
    bool panel_update_avail = panel_fw_info.available &&
        (panel_fw_info.version > ota_manager_get_current_version());
    bool main_update_avail = (rp2350_fw_status.available_version != 0);
    bool any_update = panel_update_avail || main_update_avail;

    // Use main board version as the user-facing "system version"
    ui_draw_firmware_status(&display,
                            rp2350_fw_status.current_version,
                            rp2350_fw_status.available_version,
                            any_update);
}

// Check firmware status for both panel and main board
static void check_firmware_status(void) {
    ESP_LOGI(TAG, "Checking firmware status...");

    // Show loading screen
    show_info_screen("Firmware", "Checking...");
    display_manager_update(&display);  // Force immediate display update before blocking operations

    fw_status_valid = false;
    memset(&panel_fw_info, 0, sizeof(panel_fw_info));
    memset(&rp2350_fw_status, 0, sizeof(rp2350_fw_status));

    if (!host_comm.initialized) {
        ESP_LOGW(TAG, "Host communication not initialized");
        show_info_screen("Error", "Host not connected");
        display_manager_update(&display);
        return;
    }

    // Initialize OTA manager if not already done
    esp_err_t ret = ota_manager_init(&ota_manager, &host_comm);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to initialize OTA manager: %s", esp_err_to_name(ret));
    }

    // Check for panel firmware update (ESP32 firmware from combined UF2)
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

// Trigger unified system update (panel OTA + main board update)
static void trigger_system_update(void) {
    ESP_LOGI(TAG, "Triggering system firmware update...");

    current_screen = SCREEN_FIRMWARE_UPDATE;
    ui_draw_firmware_update(&display, "Starting update...", 0);

    xTaskCreate(firmware_update_task, "firmware_update", 8192, NULL, 10, NULL);
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
    ESP_LOGI(TAG, "PicoIDE Front Panel Starting...");

    // NVS must be ready before display_manager_init loads its idle-mode/timeout
    // config. wifi_manager_init also calls nvs_flash_init later, but that's
    // idempotent.
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

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

    menu_init(&display_settings_menu, MENU_VISIBLE_ITEMS_WITH_TITLE);
    display_settings_menu.title = "Display";
    menu_set_items(&display_settings_menu, display_settings_menu_items,
                  sizeof(display_settings_menu_items) / sizeof(display_settings_menu_items[0]));

    menu_init(&idle_mode_menu, MENU_VISIBLE_ITEMS_WITH_TITLE);
    idle_mode_menu.title = "Idle Mode";
    menu_set_items(&idle_mode_menu, idle_mode_menu_items,
                  sizeof(idle_mode_menu_items) / sizeof(idle_mode_menu_items[0]));

    menu_init(&idle_timeout_menu, MENU_VISIBLE_ITEMS_WITH_TITLE);
    idle_timeout_menu.title = "Idle Timeout";
    menu_set_items(&idle_timeout_menu, idle_timeout_menu_items, IDLE_TIMEOUT_OPTION_COUNT);

    menu_init(&blank_timeout_menu, MENU_VISIBLE_ITEMS_WITH_TITLE);
    blank_timeout_menu.title = "Blank Timeout";
    menu_set_items(&blank_timeout_menu, blank_timeout_menu_items, BLANK_TIMEOUT_OPTION_COUNT);

    display_manager_set_off_state_callback(&display, on_display_off_state_change, NULL);

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
                char main_version[20];
                ota_manager_format_version_string(main_version, fw_status.current_version);
                ui_update_splash_version(&display, main_version);
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
        ESP_LOGI(TAG, "Access web interface at: http://picoide.local (when WiFi connects)");
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
