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
#include "host_link.h"  // Per-transport config + persisted transport type
#include "transport_config.h" // Transport configuration
#include "ui_screens.h"
#include "led_driver.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "interface_common.h"
#include "esp_netif_ip_addr.h"
#include "ota_manager.h"
#include "fw_version.h"
#include "driver/gpio.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "test_hooks.h"

static const char *TAG = "main";

// Main board may be held in reset for several seconds after power-on. Wait
// this long for it at startup before booting in degraded mode; a background
// probe keeps trying afterward so the panel recovers whenever it appears.
#define HOST_COMM_STARTUP_TIMEOUT_MS  20000
#define HOST_COMM_RETRY_INTERVAL_MS   500

#ifdef CONFIG_HOST_TRANSPORT_AUTO
// Auto-detect: give the NVS-saved transport this much of the startup window
// to itself before alternating both transports (covers a panel moved between
// board types without probing the other bus on every normal boot).
#define HOST_LINK_SAVED_EXCLUSIVE_MS  10000
// Background probe cadence once boot detection has given up. Backs off from
// MIN to MAX: every probe installs and removes a bus driver, and the SPI one
// shares SPI2_HOST with the OLED, so a panel left with no main board attached
// must not churn the display's bus once a second forever.
#define HOST_LINK_PROBE_MIN_MS        1000
#define HOST_LINK_PROBE_MAX_MS        30000
#endif

// OLED startup recovery. When the panel is powered only from the host (no USB),
// the +3V3 rail ramps slowly enough that the display's hardware reset (LM809)
// can release — and its +9V VPP boost stabilize — after display_manager_init()
// has already run its one-shot init, leaving the OLED dark. Re-assert the init
// a few times over the first few seconds; harmless once the panel is up.
#define DISPLAY_STARTUP_REINIT_FIRST_MS     2000
#define DISPLAY_STARTUP_REINIT_INTERVAL_MS  500
#define DISPLAY_STARTUP_REINIT_COUNT        3

// Global instances
static display_manager_t display;
static menu_t main_menu;
static menu_t disc_menu;
static menu_t wifi_menu;
static menu_t settings_menu;
static menu_t device_menu;
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
    [SCREEN_DEVICE_SELECT] = &device_menu,
    [SCREEN_DISPLAY_SETTINGS] = &display_settings_menu,
    [SCREEN_IDLE_MODE] = &idle_mode_menu,
    [SCREEN_IDLE_TIMEOUT] = &idle_timeout_menu,
    [SCREEN_BLANK_TIMEOUT] = &blank_timeout_menu,
};
static host_comm_t host_comm;
static bool host_comm_ready = false;  // True once the transport layer is up (host_comm_init succeeded)
#ifdef CONFIG_HOST_TRANSPORT_AUTO
static bool auto_link_pending = false;  // Boot detection found no main board; keep probing in the background
static host_comm_t probe_comm;  // Scratch handle for background probes; the shared host_comm
                                // (and its mutex) must never be torn down once handlers can see it
#endif
static wifi_manager_t wifi_manager;
static web_server_t web_server;
static interface_context_t interface_ctx;
static screen_type_t current_screen = SCREEN_SPLASH;
static ota_manager_t ota_manager;

// Dynamic disc list - populated from I2C communication
static bool disc_list_loaded = false;
static char current_disc_name[64] = "";
static playback_status_t current_playback_status = {0};
static bool disc_name_changed = true;  // Flag to signal title changed (resets scroll)
static bool status_needs_redraw = true;  // Flag to signal status screen needs redraw
static loaded_image_status_t current_image_status = {0};  // Full image status including directory

// Device type (IDE vs ATAPI) - affects available operations
static uint8_t current_device_type = PANEL_DEVICE_TYPE_ATAPI;  // Default to ATAPI

// Multi-device support
static uint8_t device_count = 0;
static uint16_t active_device_index = 0;
static uint8_t device_list_buffer[sizeof(device_list_response_t) + CONFIG_MAX_DEVICES * sizeof(device_summary_t)];
static device_list_response_t *device_list = (device_list_response_t *)device_list_buffer;
static bool device_list_valid = false;

// Latest PANEL_CMD_GET_DEVICE_STATUS reply, refreshed alongside playback
// status. Drives the LED color so SD error states (NO_CARD, WRONG_MODE) get
// distinct indication beyond just "no disc."
static uint8_t current_device_status = PANEL_DEVICE_STATUS_NO_IMAGE;

// Firmware status tracking
static rp2350_fw_status_t rp2350_fw_status = {0};
static panel_firmware_info_t panel_fw_info = {0};
static bool fw_status_valid = false;
#ifdef CONFIG_PRODUCT_BLUESCSI
static uint8_t fw_screen_selection = 0;  // 0=Panel, 1=Main board
#endif

// Forward declarations
static esp_err_t refresh_directory_list(void);
static void refresh_playback_status(void);
static void refresh_device_type(void);

// Web handlers change the directory (or its contents) without going through the
// panel's menus, so they invalidate the cache here and the browser reloads on
// its next open.
void interface_invalidate_disc_list(void) {
    disc_list_loaded = false;
}
static void refresh_device_list(void);
static bool select_default_device(void);
static bool active_device_is_removable(void);
static void rebuild_main_menu(void);
static void populate_device_menu(void);
static void on_host_link_established(void);
static const char* get_active_device_label(void);
static void check_firmware_status(void);
static void draw_firmware_status_screen(void);
#ifdef CONFIG_PRODUCT_BLUESCSI
static void trigger_panel_update(void);
static void trigger_mainboard_update(void);
#else
static void trigger_system_update(void);
#endif
static void reconnect_to_host(void);
static bool handle_comm_error(esp_err_t err);
static void show_info_screen(const char *title, const char *body);
static void redraw_current_screen(void);
#ifdef CONFIG_PANEL_TEST_HOOKS
static void panel_publish_ui_state(void);
static void panel_publish_framebuffer(void);
#endif

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

// Main menu contents depend on the state of the board we're controlling: "Eject
// Image" only if the current device has removeable media, and "Select Device"
// only when there is more than one device. rebuild_main_menu() manages this
#define MAIN_MENU_MAX_ITEMS 5
static menu_item_t main_menu_items[MAIN_MENU_MAX_ITEMS];
static struct {
    int8_t select_image;
    int8_t eject_image;
    int8_t select_device;
    int8_t settings;
    int8_t system_info;
} main_menu_idx;

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
             "Idle time: %s", idle_timeout_short_label(display_manager_get_idle_timeout(&display)));
    snprintf(display_settings_menu_items[2].text, MENU_ITEM_MAX_LENGTH,
             "Blank time: %s", blank_timeout_short_label(display_manager_get_off_timeout(&display)));
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
                        if (host_comm.link_up && current_image_status.image_loaded) {
                            esp_err_t ret = host_comm_select_prev_image(&host_comm, active_device_index);
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
                        if (host_comm.link_up && current_image_status.image_loaded) {
                            esp_err_t ret = host_comm_select_next_image(&host_comm, active_device_index);
                            if (ret == ESP_OK) {
                                refresh_playback_status();
                            }
                        }
                    }
                    break;
                case 3: // Back button (West) - Eject (removable media only)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        if (active_device_is_removable() && host_comm.link_up) {
                            // Show inverted icon as eject feedback
                            ui_draw_status_screen(&display, current_disc_name,
                                                  &current_image_status,
                                                  &current_playback_status,
                                                  false, get_active_device_label(),
                                                  true);
                            display_manager_update(&display);
                            esp_err_t ret = host_comm_eject_image(&host_comm, active_device_index);
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
                        int32_t selected = (int32_t)menu_get_selected_index(&main_menu);

                        if (selected == main_menu_idx.select_device) {
                            populate_device_menu();
                            current_screen = SCREEN_DEVICE_SELECT;
                        } else if (selected == main_menu_idx.select_image) {
                            // Refresh directory list before showing it
                            if (!disc_list_loaded && host_comm.link_up) {
                                ESP_LOGI(TAG, "Loading directory list...");
                                refresh_directory_list();
                            }
                            // Show target device in title when multiple devices exist
                            const char *label = get_active_device_label();
                            if (label) {
                                static char disc_title[48];
                                snprintf(disc_title, sizeof(disc_title), "%.47s", label);
                                disc_menu.title = disc_title;
                            } else {
                                disc_menu.title = "Select Image";
                            }
                            current_screen = SCREEN_DISC_LIST;
                        } else if (selected == main_menu_idx.eject_image) {
                            if (host_comm.link_up) {
                                // Show inverted icon as eject feedback
                                ui_draw_status_screen(&display, current_disc_name,
                                                      &current_image_status,
                                                      &current_playback_status,
                                                      false, get_active_device_label(),
                                                      true);
                                display_manager_update(&display);
                                esp_err_t ret = host_comm_eject_image(&host_comm, active_device_index);
                                if (ret == ESP_OK) {
                                    ESP_LOGI(TAG, "Image ejected");
                                    current_screen = SCREEN_STATUS;
                                    refresh_playback_status();
                                } else {
                                    ESP_LOGW(TAG, "Failed to eject disc: %s", esp_err_to_name(ret));
                                    handle_comm_error(ret);
                                }
                            }
                        } else if (selected == main_menu_idx.settings) {
                            current_screen = SCREEN_SETTINGS;
                        } else if (selected == main_menu_idx.system_info) {
                            info_return_screen = SCREEN_MAIN_MENU;
                            current_screen = SCREEN_INFO;
                            const esp_app_desc_t* info_app_desc = esp_app_get_description();
                            char main_ver_str[20] = "N/A";
                            if (host_comm.link_up) {
                                rp2350_fw_status_t fw_status;
                                if (host_comm_get_rp2350_fw_status(&host_comm, &fw_status) == ESP_OK) {
                                    fw_version_format_mainboard(main_ver_str, sizeof(main_ver_str), fw_status.current_version);
                                }
                            }
                            char info_text[160];
                            snprintf(info_text, sizeof(info_text),
                                     "%s\nPanel: v%s\nMain:  v%s\n%s: %s",
                                     PRODUCT_NAME_FULL,
                                     info_app_desc->version,
                                     main_ver_str,
                                     host_comm_get_transport_name(&host_comm),
                                     host_comm.link_up ? "Connected" : "Disconnected");
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
                            if (host_comm.link_up) {
                                // Use the host's real entry index recorded on the
                                // row (-1 = parent dir). Deriving it from the menu
                                // position breaks whenever the host list contains
                                // entries we skip (its own "..") — that desync
                                // loaded the entry above the one selected.
                                int32_t entry_index = selected_item->entry_index;

                                // Check if this is directory navigation (".." or "[directory]")
                                bool is_directory = (selected == 0) || (selected_item->text[0] == '[');

                                esp_err_t ret = host_comm_select_entry(&host_comm, entry_index, active_device_index);
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

        case SCREEN_DEVICE_SELECT:
            switch (event->button_id) {
                case 0: // Up button (North)
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_up(&device_menu);
                    }
                    break;
                case 2: // Down button (South)
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_down(&device_menu);
                    }
                    break;
                case 1: // Select button (East)
                    if (event->type == BUTTON_EVENT_CLICK && device_list_valid) {
                        uint32_t selected = menu_get_selected_index(&device_menu);
                        if (selected < device_list->device_count) {
                            active_device_index = device_list->devices[selected].device_index;
                            interface_ctx.active_device_index = active_device_index;
                            ESP_LOGI(TAG, "Selected device %u: %s",
                                     active_device_index,
                                     device_list->devices[selected].device_label);
                            // Eject may appear or disappear with the new device
                            rebuild_main_menu();
                            current_screen = SCREEN_STATUS;
                            refresh_playback_status();
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
#ifdef CONFIG_PRODUCT_BLUESCSI
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
#else
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
#endif
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

#ifdef CONFIG_PANEL_TEST_HOOKS
    panel_publish_ui_state();
#endif
}

#ifdef CONFIG_PANEL_TEST_HOOKS
// Keep in step with screen_type_t in ui_screens.h.
static const char *const screen_names[SCREEN_COUNT] = {
    "SCREEN_SPLASH", "SCREEN_STATUS", "SCREEN_MAIN_MENU", "SCREEN_DISC_LIST",
    "SCREEN_SETTINGS", "SCREEN_WIFI_MENU", "SCREEN_DISPLAY_SETTINGS",
    "SCREEN_IDLE_MODE", "SCREEN_IDLE_TIMEOUT", "SCREEN_BLANK_TIMEOUT",
    "SCREEN_INFO", "SCREEN_FIRMWARE_UPDATE", "SCREEN_FIRMWARE_STATUS",
    "SCREEN_DEVICE_SELECT",
};
_Static_assert(sizeof(screen_names) / sizeof(screen_names[0]) == SCREEN_COUNT,
               "screen_names is out of step with screen_type_t");

// Published snapshot. menu_clear_items() free()s menu->items (menu_system.c:83)
// and refresh_directory_list() re-adds them, so a foreign task walking the live
// menu is a use-after-free. Only the task that owns the menus fills this in;
// the web task reads the copy.
static panel_ui_state_t ui_snapshot;
static volatile uint32_t ui_snapshot_seq;   // odd while being written

// MUST be called only from the task that mutates the menus (gpio_event_task via
// handle_button_event, and refresh_directory_list on that same task).
static void panel_publish_ui_state(void) {
    ui_snapshot_seq++;                       // -> odd, readers retry
    __sync_synchronize();

    memset(&ui_snapshot, 0, sizeof(ui_snapshot));
    screen_type_t screen = current_screen;
    ui_snapshot.screen = (int)screen;
    ui_snapshot.screen_name = (screen >= 0 && screen < SCREEN_COUNT)
                            ? screen_names[screen] : "SCREEN_UNKNOWN";
    ui_snapshot.entry_index = -1;
    // A CLICK arriving while the display is not at full brightness is consumed
    // as a wake and the action is dropped (see handle_button_event), so a test
    // needs to see this to explain a press that "did nothing".
    static const char *const display_state_names[] = {
        "FULL_BRIGHTNESS", "DIMMED", "SCREENSAVER", "OFF"
    };
    int ds = (int)display.state;
    ui_snapshot.display_state = ds;
    ui_snapshot.display_state_name =
        (ds >= 0 && ds < (int)(sizeof(display_state_names)/sizeof(display_state_names[0])))
        ? display_state_names[ds] : "UNKNOWN";

    menu_t *menu = active_menu;
    if (menu) {
        ui_snapshot.has_menu = true;
        ui_snapshot.cursor = menu->selected_index;
        ui_snapshot.count = menu->item_count;
        menu_item_t *item = menu_get_selected_item(menu);
        if (item) {
            ui_snapshot.entry_index = item->entry_index;
            strncpy(ui_snapshot.selected, item->text, sizeof(ui_snapshot.selected) - 1);
            ui_snapshot.selected[sizeof(ui_snapshot.selected) - 1] = '\0';
        }
    }

    __sync_synchronize();
    ui_snapshot_seq++;                       // -> even, readers may proceed
}

void panel_get_ui_state(panel_ui_state_t *out) {
    if (!out) {
        return;
    }
    for (int attempt = 0; attempt < 4; attempt++) {
        uint32_t before = ui_snapshot_seq;
        if (before & 1u) {                   // publish in progress
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        __sync_synchronize();
        *out = ui_snapshot;
        __sync_synchronize();
        if (ui_snapshot_seq == before) {
            return;
        }
    }
    // Never hand back a torn record; say so instead.
    memset(out, 0, sizeof(*out));
    out->screen_name = "SCREEN_UNKNOWN";
    out->entry_index = -1;
}

// Shadow of the last frame actually sent to the panel. Written only by
// display_update_task, read by the web task under the same seqlock scheme as
// the UI snapshot.
static uint8_t fb_shadow[PANEL_FB_MAX_BYTES];
static volatile uint32_t fb_shadow_seq;      // odd while being written
static size_t fb_shadow_len;
static int fb_shadow_tw, fb_shadow_th, fb_shadow_rot = -1;

static void panel_publish_framebuffer(void) {
    u8g2_t *u8g2 = &display.u8g2;
    const uint8_t *buf = u8g2_GetBufferPtr(u8g2);
    if (!buf) {
        return;
    }
    int w = u8g2_GetBufferTileWidth(u8g2);
    int h = u8g2_GetBufferTileHeight(u8g2);
    size_t n = (size_t)w * (size_t)h * 8u;
    if (n > sizeof(fb_shadow)) {
        n = sizeof(fb_shadow);
    }

    fb_shadow_seq++;
    __sync_synchronize();
    memcpy(fb_shadow, buf, n);
    fb_shadow_len = n;
    fb_shadow_tw = w;
    fb_shadow_th = h;
    // U8G2_R0..R3 as an index, so the host converter does not hardcode it.
    fb_shadow_rot = (u8g2->cb == U8G2_R0) ? 0 :
                    (u8g2->cb == U8G2_R1) ? 1 :
                    (u8g2->cb == U8G2_R2) ? 2 :
                    (u8g2->cb == U8G2_R3) ? 3 : -1;
    __sync_synchronize();
    fb_shadow_seq++;
}

size_t panel_get_framebuffer(uint8_t *out, size_t cap, int *tile_w, int *tile_h, int *rotation) {
    if (!out) {
        return 0;
    }
    for (int attempt = 0; attempt < 4; attempt++) {
        uint32_t before = fb_shadow_seq;
        if (before & 1u) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        __sync_synchronize();
        size_t n = fb_shadow_len;
        if (n == 0 || n > cap) {
            return 0;
        }
        memcpy(out, fb_shadow, n);
        if (tile_w) *tile_w = fb_shadow_tw;
        if (tile_h) *tile_h = fb_shadow_th;
        if (rotation) *rotation = fb_shadow_rot;
        __sync_synchronize();
        if (fb_shadow_seq == before) {
            return n;
        }
    }
    return 0;
}
#endif

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
    static uint8_t startup_reinits = 0;
    static uint32_t next_reinit_ms = DISPLAY_STARTUP_REINIT_FIRST_MS;

    while (1) {
        if (display.initialized) {
            // Recover the OLED if it missed its power-on init on a slow
            // (host-only) power rail — re-assert the init a few times over the
            // first few seconds. Non-blocking; repaints the current buffer.
            if (startup_reinits < DISPLAY_STARTUP_REINIT_COUNT) {
                uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
                if (now_ms >= next_reinit_ms) {
                    display_manager_reinit(&display);
                    next_reinit_ms += DISPLAY_STARTUP_REINIT_INTERVAL_MS;
                    startup_reinits++;
                }
            }

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
                                          screen_changed || disc_name_changed,
                                          get_active_device_label(),
                                          false);
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

#ifdef CONFIG_PANEL_TEST_HOOKS
            // Snapshot the frame that just went to the glass. /api/screen must
            // not read the live u8g2 buffer: ui_draw_menu paints it across many
            // calls, so a web read would tear mid-frame.
            panel_publish_framebuffer();
#endif
        }

        vTaskDelay(pdMS_TO_TICKS(33)); // 30 FPS for smooth animation
    }
}

// Status screen refresh task - updates playback data from host, and
// reconnects to the main board if it was unreachable at startup
#ifdef CONFIG_HOST_TRANSPORT_AUTO
// One alternating probe for a main board, using the scratch handle. The shared
// host_comm is only ever brought up on success — web handlers may already hold
// it, so it must never be torn down here. Returns true once the link is up.
static bool auto_link_probe_once(void) {
    static transport_type_t next_type = TRANSPORT_TYPE_I2C;
    bool established = false;

    transport_config_t cfg;
    host_link_build_config(next_type, &cfg);
    if (host_comm_init(&probe_comm, &cfg) == ESP_OK) {
        bool alive = host_comm_probe_alive(&probe_comm, 0) == ESP_OK &&
                     host_comm_probe_alive(&probe_comm, 0) == ESP_OK;
        host_comm_deinit(&probe_comm);
        if (alive && host_comm_init(&host_comm, &cfg) == ESP_OK) {
            ESP_LOGI(TAG, "Main board found on %s transport, connection established",
                     host_comm_get_transport_name(&host_comm));
            auto_link_pending = false;
            host_comm_ready = true;
            host_link_save_type(next_type);
            on_host_link_established();
            established = true;
        }
    }

    next_type = (next_type == TRANSPORT_TYPE_SPI) ? TRANSPORT_TYPE_I2C
                                                  : TRANSPORT_TYPE_SPI;
    return established;
}
#endif

static void status_refresh_task(void *pvParameters) {
#ifdef CONFIG_HOST_TRANSPORT_AUTO
    uint32_t probe_backoff_ms = HOST_LINK_PROBE_MIN_MS;
    int32_t  probe_due_in_ms = 0;   // probe on the first pass
#endif
    while (1) {
        uint32_t poll_interval_ms = 1000;

#ifdef CONFIG_HOST_TRANSPORT_AUTO
        // Runs BEFORE the display-state gate: a blanked screen must not stop the
        // panel from noticing a main board powered on later, and idle blanking
        // is on by default. Backs off because each probe installs and removes a
        // bus driver — the SPI one on SPI2_HOST, shared with the OLED — and
        // doing that every second forever on a panel with no board attached is
        // both pointless and contention on a live display bus.
        if (auto_link_pending) {
            if (probe_due_in_ms <= 0) {
                if (auto_link_probe_once()) {
                    probe_backoff_ms = HOST_LINK_PROBE_MIN_MS;
                } else if (probe_backoff_ms < HOST_LINK_PROBE_MAX_MS) {
                    probe_backoff_ms *= 2;
                    if (probe_backoff_ms > HOST_LINK_PROBE_MAX_MS) {
                        probe_backoff_ms = HOST_LINK_PROBE_MAX_MS;
                    }
                }
                probe_due_in_ms = (int32_t)probe_backoff_ms;
            }
        } else {
            probe_backoff_ms = HOST_LINK_PROBE_MIN_MS;
            probe_due_in_ms = 0;
        }
#endif

        if (display.state == DISPLAY_STATE_OFF) {
            // Screen off: no need to poll at all
            poll_interval_ms = 0;
        } else if (current_screen == SCREEN_STATUS && host_comm.link_up) {
            if (current_device_type == PANEL_DEVICE_TYPE_IDE ||
                current_device_type == PANEL_DEVICE_TYPE_SCSI) {
                // HDD: image can only change via web UI, poll infrequently
                poll_interval_ms = 5000;
            } else if (!current_playback_status.is_playing) {
                // ATAPI with no audio playing: poll less frequently
                poll_interval_ms = 5000;
            } else {
                // ATAPI with audio playing: fast poll for track position updates
                poll_interval_ms = 1000;
            }
            refresh_playback_status();
        } else if (host_comm_ready && !host_comm.link_up) {
            // Main board was unreachable at startup (e.g. held in reset).
            // Keep probing so the panel recovers once it comes online.
            //
            // Only while the link is actually down. Without the !link_up
            // test this ran on every pass whenever the status screen was not
            // showing, re-announcing a healthy link once a second and calling
            // on_host_link_established(), which resets the menu selection to 0
            // and drops the cached directory listing under the user.
            uint8_t device_status;
            if (host_comm_get_device_status(&host_comm, 0, &device_status) == ESP_OK) {
                ESP_LOGI(TAG, "Main board is now responding, connection restored");
                host_comm.link_up = true;
                on_host_link_established();
            }
        }

        uint32_t delay_ms = poll_interval_ms ? poll_interval_ms : 10000;
#ifdef CONFIG_HOST_TRANSPORT_AUTO
        // Never sleep past the next probe, and charge the sleep against it.
        if (auto_link_pending && (uint32_t)probe_due_in_ms < delay_ms) {
            delay_ms = (probe_due_in_ms > 100) ? (uint32_t)probe_due_in_ms : 100;
        }
        probe_due_in_ms -= (int32_t)delay_ms;
#endif
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

// Function to refresh playback status from host
static void refresh_playback_status(void) {
    char old_disc_name[64];
    strncpy(old_disc_name, current_disc_name, sizeof(old_disc_name) - 1);
    old_disc_name[sizeof(old_disc_name) - 1] = '\0';

    // The playback status poll doubles as the liveness probe by checking for
    // PANEL_ALIVE_MAGIC. Poll the active device so the disc name, playback
    // state, and device status all describe the same device.
    esp_err_t ret = host_comm.link_up
        ? host_comm_get_playback_status(&host_comm, active_device_index, &current_playback_status)
        : ESP_FAIL;
    if (ret != ESP_OK ||
        current_playback_status.alive_magic != PANEL_ALIVE_MAGIC) {
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

    // The main board is alive. If we didn't get a device list at startup, fetch
    // it now Note that an empty device list is possible, for example with
    // BlueSCSI in initiator mode
    if (!device_list_valid) {
        refresh_device_list();
        if (device_list_valid && select_default_device()) {
            refresh_device_type();
        }
    }

    if (current_playback_status.disc_name[0]) {
        // Trust whatever string the main board put in disc_name — it carries the
        // image name when a disc is loaded (the main board caches the filename in
        // its main loop), and the error text ("No SD card" / "Wrong-mode card")
        // when in an SD error state.
        strncpy(current_disc_name, current_playback_status.disc_name,
                sizeof(current_disc_name) - 1);
        current_disc_name[sizeof(current_disc_name) - 1] = '\0';
    } else {
        strncpy(current_disc_name, "No image loaded", sizeof(current_disc_name) - 1);
        current_disc_name[sizeof(current_disc_name) - 1] = '\0';
    }

    // Track recovery from an SD error so a stale directory listing from the old
    // card gets refetched. device_status rides along in the playback status.
    uint8_t prev = current_device_status;
    current_device_status = current_playback_status.device_status;
    bool prev_err = (prev == PANEL_DEVICE_STATUS_NO_CARD ||
                     prev == PANEL_DEVICE_STATUS_WRONG_MODE);
    bool now_err  = (current_device_status == PANEL_DEVICE_STATUS_NO_CARD ||
                     current_device_status == PANEL_DEVICE_STATUS_WRONG_MODE);
    if (prev_err && !now_err) {
        ESP_LOGI(TAG, "SD card recovered; invalidating disc list cache");
        disc_list_loaded = false;
    }

    // Check if disc name actually changed (for fetching full image status and resetting scroll)
    bool name_changed = (strcmp(old_disc_name, current_disc_name) != 0);

    if (name_changed) {
        disc_name_changed = true;  // Reset scroll animation
        // Fetch loaded image status if not already done above
        if (current_playback_status.disc_name[0]) {
            ret = host_comm_get_loaded_image_status(&host_comm, active_device_index, &current_image_status);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to get loaded image status: %s", esp_err_to_name(ret));
                memset(&current_image_status, 0, sizeof(current_image_status));
            } else {
                current_device_type = current_image_status.device_type;
            }
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
    if (!host_comm.link_up) {
        return;
    }

    loaded_image_status_t status;
    esp_err_t ret = host_comm_get_loaded_image_status(&host_comm, active_device_index, &status);
    if (ret == ESP_OK) {
        current_device_type = status.device_type;
        ESP_LOGI(TAG, "Device type: %u", current_device_type);
        rebuild_main_menu();
    }
}

// Function to refresh device list from host
static void refresh_device_list(void) {
    if (!host_comm.link_up) {
        return;
    }

    esp_err_t ret = host_comm_get_device_list(&host_comm, device_list, sizeof(device_list_buffer));
    if (ret == ESP_OK) {
        device_count = device_list->device_count;
        device_list_valid = true;
        ESP_LOGI(TAG, "Device list: %u devices (max %u)", device_count, device_list->max_devices);
    } else {
        ESP_LOGW(TAG, "Failed to get device list: %s (single device assumed)", esp_err_to_name(ret));
        // Assume single device if command not supported
        device_count = 1;
        device_list_valid = false;
    }
    rebuild_main_menu();
}

// Device types whose media is removable. category a future main board reports
// that we do not know about - is never treated as ejectable. These are the
// devices a user is most likely to want selected on the panel. Mirrors
// EJECTABLE_TYPES in www/app.js: keep the two in sync.
static bool device_type_is_removable(uint8_t device_type) {
    switch (device_type) {
        case PANEL_DEV_CATEGORY_REMOVABLE:
        case PANEL_DEV_CATEGORY_OPTICAL:
        case PANEL_DEV_CATEGORY_FLOPPY:
        case PANEL_DEV_CATEGORY_MO:
        case PANEL_DEV_CATEGORY_ZIP:
            return true;
        default:
            return false;
    }
}

// Whether the active device's media is removable.
static bool active_device_is_removable(void) {
    if (device_list_valid) {
        for (uint8_t i = 0; i < device_list->device_count; i++) {
            if (device_list->devices[i].device_index == active_device_index) {
                return device_type_is_removable(device_list->devices[i].device_type);
            }
        }
    }
    return current_device_type == PANEL_DEVICE_TYPE_ATAPI;
}

static void main_menu_append(uint32_t *count, const char *text, int8_t *slot) {
    menu_item_t *item = &main_menu_items[*count];
    strncpy(item->text, text, MENU_ITEM_MAX_LENGTH - 1);
    item->text[MENU_ITEM_MAX_LENGTH - 1] = '\0';
    item->action = MENU_ACTION_CUSTOM;
    item->selectable = true;
    *slot = (int8_t)(*count);
    (*count)++;
}

// Rebuild the main menu for the currently active device.
static void rebuild_main_menu(void) {
    static bool built = false;
    static bool built_ejectable = false;
    static bool built_multi = false;

    bool ejectable = active_device_is_removable();
    bool multi = (device_count > 1);
    if (built && ejectable == built_ejectable && multi == built_multi) {
        return;
    }
    built = true;
    built_ejectable = ejectable;
    built_multi = multi;

    main_menu_idx.select_image = -1;
    main_menu_idx.eject_image = -1;
    main_menu_idx.select_device = -1;
    main_menu_idx.settings = -1;
    main_menu_idx.system_info = -1;

    uint32_t count = 0;
    main_menu_append(&count, "Select Image", &main_menu_idx.select_image);
    if (ejectable) {
        main_menu_append(&count, "Eject Image", &main_menu_idx.eject_image);
    }
    if (multi) {
        main_menu_append(&count, "Select Device", &main_menu_idx.select_device);
    }
    main_menu_append(&count, "Settings", &main_menu_idx.settings);
    main_menu_append(&count, "System Info", &main_menu_idx.system_info);

    menu_set_items(&main_menu, main_menu_items, count);
}

// Choose the device shown on the panel by default. Prefer the first removable
// device (e.g. a CD-ROM) over device 0, which is commonly a fixed HDD or a
// non-removable device such as a network adapter. Falls back to the first
// listed device when none are removable. Returns false when the list is empty
// (e.g. a BlueSCSI in initiator mode) and no device was picked.
static bool select_default_device(void) {
    if (!device_list_valid || device_list->device_count == 0) {
        return false;
    }

    for (uint8_t i = 0; i < device_list->device_count; i++) {
        if (device_type_is_removable(device_list->devices[i].device_type)) {
            active_device_index = device_list->devices[i].device_index;
            interface_ctx.active_device_index = active_device_index;
            ESP_LOGI(TAG, "Default device -> removable %u (%s)",
                     active_device_index, device_list->devices[i].device_label);
            return true;
        }
    }

    // No removable device present: default to the first listed device.
    active_device_index = device_list->devices[0].device_index;
    interface_ctx.active_device_index = active_device_index;
    return true;
}

// Populate the device select menu from cached device list
static void populate_device_menu(void) {
    menu_clear_items(&device_menu);

    if (!device_list_valid || device_count == 0) {
        menu_add_item(&device_menu, "No devices", MENU_ACTION_SELECT, NULL, NULL);
        return;
    }

    for (uint8_t i = 0; i < device_list->device_count; i++) {
        char label[MENU_ITEM_MAX_LENGTH];
        if (device_list->devices[i].image_name[0]) {
            snprintf(label, sizeof(label), "%.28s: %.32s",
                     device_list->devices[i].device_label,
                     device_list->devices[i].image_name);
        } else {
            snprintf(label, sizeof(label), "%.28s: [empty]",
                     device_list->devices[i].device_label);
        }
        menu_add_item(&device_menu, label, MENU_ACTION_SELECT, NULL, NULL);
    }
}

// Get the device label for the currently active device (for status screen header)
static const char* get_active_device_label(void) {
    if (device_count <= 1 || !device_list_valid) {
        return NULL;  // Single device, no label needed
    }

    for (uint8_t i = 0; i < device_list->device_count; i++) {
        if (device_list->devices[i].device_index == active_device_index) {
            return device_list->devices[i].device_label;
        }
    }
    return NULL;
}

// Bring the UI into sync with a main board that has just answered. Boot-time
// detection did this inline, so a board that appeared LATER (the normal case for
// a USB-powered panel) came up half-initialized: no device list, so the
// device-select screen stayed inert, and current_device_type stuck at its
// default of ATAPI, giving CD affordances and CD poll cadence on an HDD-only
// board. Called from every path that establishes the link.
static void on_host_link_established(void) {
    // Settle on the active device BEFORE reading its type, so current_device_type
    // describes the device we actually landed on. refresh_device_list() and
    // refresh_device_type() each call rebuild_main_menu(), and the last one wins:
    // it decides the "Eject Image" row from the settled device's removability and
    // the "Select Device" row from the device count.
    refresh_device_list();
    select_default_device();
    refresh_device_type();

    if (device_count > 1) {
        ESP_LOGI(TAG, "Multi-device mode: %u devices", device_count);
    }

    disc_list_loaded = false;   // Force directory reload when next opened
    status_needs_redraw = true; // Refresh the status screen
}

// Record the host directory-entry index on the most recently added menu row.
// menu_add_item() has no field for caller payload, so we set it after the fact.
static void set_last_menu_entry_index(menu_t *menu, int32_t entry_index) {
    if (menu && menu->items && menu->item_count > 0) {
        menu->items[menu->item_count - 1].entry_index = entry_index;
    }
}

// Function to refresh directory entry list from host
static esp_err_t refresh_directory_list(void) {
    if (!host_comm.link_up) {
        ESP_LOGW(TAG, "Main board not answering");
        return ESP_ERR_INVALID_STATE;
    }

    // Clear current menu
    menu_clear_items(&disc_menu);

    // Get entry count from host
    uint32_t entry_count = 0;
    esp_err_t ret = host_comm_get_entry_count(&host_comm, active_device_index, &entry_count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get entry count: %s", esp_err_to_name(ret));
        // Add fallback message
        menu_add_item(&disc_menu, "No entries available", MENU_ACTION_SELECT, NULL, NULL);
        // Selecting this row still sends SELECT_ENTRY; without an explicit -1 it
        // would send the default 0 and load a real image the user never picked.
        set_last_menu_entry_index(&disc_menu, -1);
        return ret;
    }

    ESP_LOGI(TAG, "Found %lu entries in current directory", entry_count);

    if (entry_count == 0) {
        menu_add_item(&disc_menu, "Empty directory", MENU_ACTION_SELECT, NULL, NULL);
        set_last_menu_entry_index(&disc_menu, -1);
        return ESP_OK;
    }

    // Add ".." entry to go to parent directory (host parent-dir sentinel)
    menu_add_item(&disc_menu, "..", MENU_ACTION_SELECT, NULL, NULL);
    set_last_menu_entry_index(&disc_menu, -1);

    // Get info for each entry (directories and files). The host's real entry
    // index is recorded on each menu row: the host injects its own ".." at
    // index 0 in every non-root directory (and we skip it, since we add our
    // own above), so the menu row no longer lines up 1:1 with the host index.
    // Selection must send the recorded index, not the row position.
    for (uint32_t i = 0; i < entry_count && i < MENU_MAX_ITEMS - 1; i++) {
        dir_entry_info_t entry_info;
        ESP_LOGI(TAG, "Requesting entry info for index %lu", i);
        ret = host_comm_get_entry_info(&host_comm, active_device_index, i, &entry_info);
        if (ret == ESP_OK) {
            // Skip ".." and "." entries — we already added our own ".." above
            if (strcmp(entry_info.name, "..") == 0 || strcmp(entry_info.name, ".") == 0) {
                continue;
            }
            // Add prefix to distinguish directories from files
            char display_name[68];  // 64 + prefix + null
            if (entry_info.entry_type == 0) {  // DIRECTORY
                snprintf(display_name, sizeof(display_name), "[%s]", entry_info.name);
            } else {
                snprintf(display_name, sizeof(display_name), "%s", entry_info.name);
            }

            ESP_LOGI(TAG, "Entry %lu: %s (type=%u)", i, entry_info.name, entry_info.entry_type);
            menu_add_item(&disc_menu, display_name, MENU_ACTION_SELECT, NULL, NULL);
            set_last_menu_entry_index(&disc_menu, (int32_t)i);
        } else {
            ESP_LOGW(TAG, "Failed to get info for entry %lu: %s", i, esp_err_to_name(ret));
            char fallback_name[32];
            snprintf(fallback_name, sizeof(fallback_name), "Entry %lu (error)", i);
            menu_add_item(&disc_menu, fallback_name, MENU_ACTION_SELECT, NULL, NULL);
            set_last_menu_entry_index(&disc_menu, (int32_t)i);
        }
    }

    disc_list_loaded = true;
    ESP_LOGI(TAG, "Directory list refreshed successfully");
#ifdef CONFIG_PANEL_TEST_HOOKS
    // The items array was freed and rebuilt above, so republish before any
    // reader can observe the new one.
    panel_publish_ui_state();
#endif
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
        host_comm.link_up = false;
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
            return host_comm.link_up;
        default:
            return false;
    }
}

// Firmware update task
#ifdef CONFIG_PRODUCT_BLUESCSI
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
#else
// Unified firmware update task: panel OTA (if needed) -> RP2350 update -> wait -> reboot self
static void unified_firmware_update_task(void *pvParameters) {
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
                fw_version_format_mainboard(version_str, sizeof(version_str), fw_status.current_version);
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
#endif

static void draw_firmware_status_screen(void) {
#ifdef CONFIG_PRODUCT_BLUESCSI
    uint32_t panel_current = ota_manager_get_current_version();
    ui_draw_firmware_status(&display,
                            panel_current, panel_fw_info.version, panel_fw_info.available,
                            rp2350_fw_status.current_version, rp2350_fw_status.available_version,
                            rp2350_fw_status.available_version != 0,
                            fw_screen_selection);
#else
    bool panel_update_avail = panel_fw_info.available &&
        (panel_fw_info.version > ota_manager_get_current_version());
    bool main_update_avail = (rp2350_fw_status.available_version != 0);
    bool any_update = panel_update_avail || main_update_avail;

    // Use main board version as the user-facing "system version"
    ui_draw_firmware_status(&display,
                            rp2350_fw_status.current_version,
                            rp2350_fw_status.available_version,
                            any_update);
#endif
}

// Check firmware status for both panel and main board
static void check_firmware_status(void) {
    ESP_LOGI(TAG, "Checking firmware status...");

    // Show loading screen
    show_info_screen("Firmware", "Checking...");
    display_manager_update(&display);  // Force immediate display update before blocking operations

    fw_status_valid = false;
#ifdef CONFIG_PRODUCT_BLUESCSI
    fw_screen_selection = 0;
#endif
    memset(&panel_fw_info, 0, sizeof(panel_fw_info));
    memset(&rp2350_fw_status, 0, sizeof(rp2350_fw_status));

    if (!host_comm.link_up) {
        ESP_LOGW(TAG, "Main board not answering");
        show_info_screen("Error", "Host not connected");
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


#ifdef CONFIG_PRODUCT_BLUESCSI
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
    fw_version_format_panel(version_str, sizeof(version_str), panel_fw_info.version);
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
            fw_version_format_mainboard(version_str, sizeof(version_str), fw_status.current_version);

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
#else
// Trigger unified system update (panel OTA + main board update)
static void trigger_system_update(void) {
    ESP_LOGI(TAG, "Triggering system firmware update...");

    current_screen = SCREEN_FIRMWARE_UPDATE;
    ui_draw_firmware_update(&display, "Starting update...", 0);

    xTaskCreate(unified_firmware_update_task, "firmware_update", 8192, NULL, 10, NULL);
}
#endif

// Host communication task (currently unused)
#if 0
static void host_comm_task(void *pvParameters) {
    // Wait a bit for system to stabilize
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    while (1) {
        if (host_comm.link_up) {
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

// Bring up the host link and verify a live main board is answering, retrying
// within the startup window (some systems hold the board in reset for several
// seconds after power-on). Forced-transport builds init the compile-time
// transport and probe it. AUTO builds (BlueSCSI) probe with alive-magic
// validation — an absent SPI host "answers" reads with floating-bus garbage —
// giving the NVS-saved transport the first part of the window to itself, then
// alternating both; the winner is persisted. On ESP_OK the shared host_comm is
// up and verified; on failure host_comm_ready reflects whether it is up at all.
static esp_err_t establish_host_link(void) {
#ifdef CONFIG_HOST_TRANSPORT_AUTO
    transport_type_t saved_type = TRANSPORT_TYPE_SPI;
    bool have_saved = host_link_load_type(&saved_type);
    // Without a saved type, try I2C first: an absent I2C slave NACKs
    // immediately, while an absent SPI host needs the magic check to reject it.
    transport_type_t current = have_saved ? saved_type : TRANSPORT_TYPE_I2C;
    ESP_LOGI(TAG, "Detecting host transport (saved: %s)",
             !have_saved ? "none" : (saved_type == TRANSPORT_TYPE_I2C ? "I2C" : "SPI"));

    uint32_t start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    while (1) {
        transport_config_t cfg;
        host_link_build_config(current, &cfg);
        // Probe on the scratch handle, never the shared one: host_comm_deinit()
        // deletes host_comm.mutex and leaves it NULL, and the web server is
        // started even when detection times out. Tearing down the shared handle
        // here left every host_comm_* caller locking a NULL semaphore. Same
        // discipline as status_refresh_task's background probe.
        if (host_comm_init(&probe_comm, &cfg) == ESP_OK) {
            // Probe twice: a floating SPI bus could clock in one
            // valid-looking alive magic by chance.
            bool alive = host_comm_probe_alive(&probe_comm, 0) == ESP_OK &&
                         host_comm_probe_alive(&probe_comm, 0) == ESP_OK;
            host_comm_deinit(&probe_comm);
            if (alive && host_comm_init(&host_comm, &cfg) == ESP_OK) {
                host_comm_ready = true;
                ESP_LOGI(TAG, "Main board found on %s transport",
                         host_comm_get_transport_name(&host_comm));
                if (!have_saved || current != saved_type) {
                    host_link_save_type(current);
                }
                return ESP_OK;
            }
        }

        uint32_t elapsed = xTaskGetTickCount() * portTICK_PERIOD_MS - start_ms;
        if (elapsed >= HOST_COMM_STARTUP_TIMEOUT_MS) {
            return ESP_ERR_TIMEOUT;
        }
        if (!have_saved || elapsed >= HOST_LINK_SAVED_EXCLUSIVE_MS) {
            current = (current == TRANSPORT_TYPE_SPI) ? TRANSPORT_TYPE_I2C
                                                      : TRANSPORT_TYPE_SPI;
        }
        ui_update_splash_progress(&display, "Wait for main board", 60);
        vTaskDelay(pdMS_TO_TICKS(HOST_COMM_RETRY_INTERVAL_MS));
    }
#else
    transport_config_t transport_cfg;
#ifdef CONFIG_HOST_TRANSPORT_I2C
    host_link_build_config(TRANSPORT_TYPE_I2C, &transport_cfg);
#else
    host_link_build_config(TRANSPORT_TYPE_SPI, &transport_cfg);
#endif

    esp_err_t ret = host_comm_init(&host_comm, &transport_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize host comm: %s", esp_err_to_name(ret));
        return ret;
    }
    host_comm_ready = true;
    ESP_LOGI(TAG, "Host comm initialized successfully using %s",
             host_comm_get_transport_name(&host_comm));

    uint8_t test_status;
    esp_err_t comm_ret = host_comm_get_device_status(&host_comm, 0, &test_status);
    if (comm_ret != ESP_OK) {
        ESP_LOGW(TAG, "Main board not responding, waiting for it to leave reset...");
        uint32_t start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        while (comm_ret != ESP_OK &&
               (xTaskGetTickCount() * portTICK_PERIOD_MS - start_ms) < HOST_COMM_STARTUP_TIMEOUT_MS) {
            ui_update_splash_progress(&display, "Wait for main board", 60);
            vTaskDelay(pdMS_TO_TICKS(HOST_COMM_RETRY_INTERVAL_MS));
            comm_ret = host_comm_get_device_status(&host_comm, 0, &test_status);
        }
    }
    return comm_ret;
#endif
}

void app_main(void) {
    ESP_LOGI(TAG, "%s Starting...", PRODUCT_NAME_FULL);

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
    rebuild_main_menu();

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

    menu_init(&device_menu, MENU_VISIBLE_ITEMS_WITH_TITLE);
    device_menu.title = "Select Device";

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

    ui_update_splash_progress(&display, "Establish comms...", 55);
    ESP_LOGI(TAG, "Waiting %d ms for main board startup...", HOST_STARTUP_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(HOST_STARTUP_DELAY_MS));

    esp_err_t comm_ret = establish_host_link();
    {
#ifdef CONFIG_HOST_TRANSPORT_AUTO
        // Detection exhausted the window with no live board on either
        // transport; keep probing from status_refresh_task.
        bool board_missing = (comm_ret != ESP_OK);
        auto_link_pending = board_missing;
#else
        // Transport up but board silent. (If host_comm_init itself failed,
        // there is nothing to probe and no blink, as before.)
        bool board_missing = (comm_ret != ESP_OK) && host_comm_ready;
#endif
        if (board_missing) {
            ESP_LOGE(TAG, "Failed to communicate with main board: %s", esp_err_to_name(comm_ret));
            led_stop_pulse();

            for (int i = 0; i < 6; i++) {
                led_set_color(COLOR_RED);
                vTaskDelay(pdMS_TO_TICKS(200));
                led_clear();
                vTaskDelay(pdMS_TO_TICKS(200));
            }

            if (host_comm_ready) {
                host_comm.link_up = false;
            }
        }
        if (comm_ret == ESP_OK) {
            ESP_LOGI(TAG, "Communication with main board verified");

            // Get and display main board firmware version
            rp2350_fw_status_t fw_status;
            if (host_comm_get_rp2350_fw_status(&host_comm, &fw_status) == ESP_OK) {
                char main_version[20];
                fw_version_format_mainboard(main_version, sizeof(main_version), fw_status.current_version);
                ui_update_splash_version(&display, main_version);
                ESP_LOGI(TAG, "Main board firmware version: %s", main_version);
            }

            // Get device list for multi-device support
            refresh_device_list();

            // Default the panel to the first removable device (e.g. CD-ROM)
            // rather than device 0, which is often a fixed HDD or a
            // non-removable device like a network adapter.
            select_default_device();

            // Get device type of the device we settled on
            refresh_device_type();

            if (device_count > 1) {
                ESP_LOGI(TAG, "Multi-device mode: %u devices", device_count);
            }

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
    interface_ctx.active_device_index = active_device_index;

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
                          &current_playback_status, disc_name_changed,
                          get_active_device_label(),
                          false);
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
