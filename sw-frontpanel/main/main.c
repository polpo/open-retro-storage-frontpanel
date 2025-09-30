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
static bool disc_name_changed = true;  // Flag to signal title changed

// Forward declarations
static esp_err_t refresh_disc_list(void);
static void refresh_playback_status(void);

static menu_item_t main_menu_items[] = {
    {.text = "Select Disc", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "Eject Disc", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "Settings", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "System Info", .action = MENU_ACTION_CUSTOM, .selectable = true},
};

static menu_item_t settings_menu_items[] = {
    {.text = "WiFi Setup", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "Web Interface", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "Display Settings", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "Back", .action = MENU_ACTION_BACK, .selectable = true},
};

static menu_item_t wifi_menu_items[] = {
    {.text = "Scan Networks", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "WiFi Status", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "Start Web Portal", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "Disconnect", .action = MENU_ACTION_CUSTOM, .selectable = true},
    {.text = "Back", .action = MENU_ACTION_BACK, .selectable = true},
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
        case SCREEN_STATUS:
            switch (event->button_id) {
                case 1: // Right button (East) - Go to main menu
                    if (event->type == BUTTON_EVENT_CLICK) {
                        current_screen = SCREEN_MAIN_MENU;
                        active_menu = &main_menu;
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
                        if (selected == 0) { // "Select Disc"
                            // Refresh disc list before showing it
                            if (!disc_list_loaded && host_comm.initialized) {
                                ESP_LOGI(TAG, "Loading disc list...");
                                refresh_disc_list();
                            }
                            current_screen = SCREEN_DISC_LIST;
                            active_menu = &disc_menu;
                        } else if (selected == 1) { // "Eject Disc"
                            if (host_comm.initialized) {
                                esp_err_t ret = host_comm_eject_disc(&host_comm);
                                if (ret == ESP_OK) {
                                    strncpy(current_disc_name, "No disc loaded", sizeof(current_disc_name) - 1);
                                    current_disc_name[sizeof(current_disc_name) - 1] = '\0';
                                    ui_draw_status_bar(&display, current_disc_name);
                                    ESP_LOGI(TAG, "Disc ejected");
                                } else {
                                    ESP_LOGW(TAG, "Failed to eject disc: %s", esp_err_to_name(ret));
                                }
                            }
                        } else if (selected == 2) { // "Settings"
                            current_screen = SCREEN_SETTINGS;
                            active_menu = &settings_menu;
                        } else if (selected == 3) { // "System Info"
                            current_screen = SCREEN_INFO;
                            active_menu = NULL;
                            char info_text[128];
                            snprintf(info_text, sizeof(info_text),
                                "PicoIDE Front Panel\nFW: v0.1.0\nESP32-C3\n%s: %s",
                                host_comm_get_transport_name(&host_comm),
                                host_comm.initialized ? "Connected" : "Disconnected");
                            ui_draw_info_screen(&display, "System Info", info_text);
                        }
                    }
                    break;
                case 3: // Back button (West)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        current_screen = SCREEN_STATUS;
                        active_menu = NULL;
                        refresh_playback_status();
                        ui_draw_status_screen(&display, current_disc_name, &current_playback_status, disc_name_changed);
                        disc_name_changed = false;
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
                            ESP_LOGI(TAG, "Selected disc: %s", selected_item->text);
                            // Send command to host device
                            if (host_comm.initialized) {
                                esp_err_t ret = host_comm_select_disc(&host_comm, selected);
                                if (ret == ESP_OK) {
                                    strncpy(current_disc_name, selected_item->text, sizeof(current_disc_name) - 1);
                                    current_disc_name[sizeof(current_disc_name) - 1] = '\0';
                                } else {
                                    ESP_LOGW(TAG, "Failed to select disc via host comm: %s", esp_err_to_name(ret));
                                }
                            }
                            // Go back to main menu
                            current_screen = SCREEN_MAIN_MENU;
                            active_menu = &main_menu;
                            ui_draw_status_bar(&display, current_disc_name);
                        }
                    }
                    break;
                case 3: // Back button (West)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        current_screen = SCREEN_MAIN_MENU;
                        active_menu = &main_menu;
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
                        if (selected == 0) { // "WiFi Setup"
                            current_screen = SCREEN_WIFI_MENU;
                            active_menu = &wifi_menu;
                        } else if (selected == 1) { // "Web Interface"
                            // TODO: Implement web interface settings
                            ESP_LOGI(TAG, "Web interface settings not implemented yet");
                        } else if (selected == 2) { // "Display Settings"
                            // TODO: Implement display settings
                            ESP_LOGI(TAG, "Display settings not implemented yet");
                        } else if (selected == 3) { // "Back"
                            current_screen = SCREEN_MAIN_MENU;
                            active_menu = &main_menu;
                        }
                    }
                    break;
                case 3: // Back button (West)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        current_screen = SCREEN_MAIN_MENU;
                        active_menu = &main_menu;
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
                        if (selected == 0) { // "Scan Networks"
                            // TODO: Implement WiFi network scanning and display
                            ESP_LOGI(TAG, "Scanning WiFi networks...");
                            char info_text[256];
                            snprintf(info_text, sizeof(info_text), "Scanning WiFi networks...\nThis feature will show\navailable networks and\nallow connection.");
                            ui_draw_info_screen(&display, "WiFi Scan", info_text);
                        } else if (selected == 1) { // "WiFi Status"
                            wifi_manager_state_t state = wifi_manager_get_state(&wifi_manager);
                            char info_text[256];
                            if (wifi_manager_is_connected(&wifi_manager)) {
                                esp_ip4_addr_t ip;
                                if (wifi_manager_get_ip_info(&wifi_manager, &ip, NULL, NULL) == ESP_OK) {
                                    snprintf(info_text, sizeof(info_text),
                                        "WiFi Status: %s\nIP Address: " IPSTR,
                                        wifi_manager_state_to_string(state), IP2STR(&ip));
                                } else {
                                    snprintf(info_text, sizeof(info_text),
                                        "WiFi Status: %s\nIP: Unknown",
                                        wifi_manager_state_to_string(state));
                                }
                            } else {
                                snprintf(info_text, sizeof(info_text),
                                    "WiFi Status: %s",
                                    wifi_manager_state_to_string(state));
                            }
                            ui_draw_info_screen(&display, "WiFi Status", info_text);
                        } else if (selected == 2) { // "Start Web Portal"
                            if (!web_server_is_running(&web_server)) {
                                esp_err_t ret = web_server_start(&web_server);
                                if (ret == ESP_OK) {
                                    ESP_LOGI(TAG, "Web server started");
                                    char info_text[256];
                                    if (wifi_manager_is_connected(&wifi_manager)) {
                                        esp_ip4_addr_t ip;
                                        if (wifi_manager_get_ip_info(&wifi_manager, &ip, NULL, NULL) == ESP_OK) {
                                            snprintf(info_text, sizeof(info_text),
                                                "Web interface started!\nAccess at:\nhttp://picoide.local\nor http://" IPSTR,
                                                IP2STR(&ip));
                                        } else {
                                            snprintf(info_text, sizeof(info_text), "Web interface started!\nCheck WiFi status for IP");
                                        }
                                    } else {
                                        snprintf(info_text, sizeof(info_text),
                                            "Web interface started!\nConnect to AP:\n%s\nThen visit:\nhttp://picoide.local\nor http://192.168.4.1",
                                            WIFI_MANAGER_AP_SSID);
                                    }
                                    ui_draw_info_screen(&display, "Web Portal", info_text);
                                } else {
                                    ESP_LOGW(TAG, "Failed to start web server: %s", esp_err_to_name(ret));
                                }
                            } else {
                                ESP_LOGI(TAG, "Web server already running");
                            }
                        } else if (selected == 3) { // "Disconnect"
                            esp_err_t ret = wifi_manager_disconnect(&wifi_manager);
                            if (ret == ESP_OK) {
                                ESP_LOGI(TAG, "WiFi disconnected");
                            } else {
                                ESP_LOGW(TAG, "Failed to disconnect WiFi: %s", esp_err_to_name(ret));
                            }
                        } else if (selected == 4) { // "Back"
                            current_screen = SCREEN_SETTINGS;
                            active_menu = &settings_menu;
                        }
                    }
                    break;
                case 3: // Back button (West)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        current_screen = SCREEN_SETTINGS;
                        active_menu = &settings_menu;
                    }
                    break;
            }
            break;

        case SCREEN_INFO:
            if (event->type == BUTTON_EVENT_CLICK && event->button_id == 3) {
                // Back button - return to main menu
                current_screen = SCREEN_MAIN_MENU;
                active_menu = &main_menu;
                ui_draw_menu(&display, &main_menu);
            }
            break;

        case SCREEN_FIRMWARE_UPDATE:
            // During firmware update, ignore all button presses
            ESP_LOGI(TAG, "Firmware update in progress - ignoring button input");
            break;

        default:
            break;
    }

    // Trigger redraw of active menu when switching screens
    if (active_menu) {
        active_menu->needs_redraw = true;
    }
}

// Display update task
static void display_update_task(void *pvParameters) {
    while (1) {
        if (display.initialized) {
            // Handle screen-specific continuous redraws for scrolling
            if (current_screen == SCREEN_STATUS) {
                // Status screen scrolling
                ui_draw_status_screen(&display, current_disc_name, &current_playback_status, disc_name_changed);
                disc_name_changed = false;
            } else if (active_menu && menu_needs_redraw(active_menu)) {
                // Menu scrolling
                ui_draw_menu(&display, active_menu);
            }

            // Send buffer to display if needed
            if (display.needs_update) {
                display_manager_update(&display);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(33)); // 30 FPS to avoid visual glitches
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
        strncpy(current_disc_name, "No disc loaded", sizeof(current_disc_name) - 1);
        current_disc_name[sizeof(current_disc_name) - 1] = '\0';
        disc_name_changed = (strcmp(old_disc_name, current_disc_name) != 0);
        return;
    }

    esp_err_t ret = host_comm_get_playback_status(&host_comm, &current_playback_status);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get playback status: %s", esp_err_to_name(ret));
        memset(&current_playback_status, 0, sizeof(current_playback_status));
        strncpy(current_disc_name, "No disc loaded", sizeof(current_disc_name) - 1);
        current_disc_name[sizeof(current_disc_name) - 1] = '\0';
        disc_name_changed = (strcmp(old_disc_name, current_disc_name) != 0);
    } else if (!current_playback_status.disc_inserted) {
        ESP_LOGI(TAG, "Playback status: No disc inserted");
        strncpy(current_disc_name, "No disc loaded", sizeof(current_disc_name) - 1);
        current_disc_name[sizeof(current_disc_name) - 1] = '\0';
        disc_name_changed = (strcmp(old_disc_name, current_disc_name) != 0);
    } else {
        // Copy disc name from playback status
        strncpy(current_disc_name, current_playback_status.disc_name, sizeof(current_disc_name) - 1);
        current_disc_name[sizeof(current_disc_name) - 1] = '\0';
        disc_name_changed = (strcmp(old_disc_name, current_disc_name) != 0);

        ESP_LOGI(TAG, "Playback status: disc='%s' type=%d playing=%d audio_status=0x%02x track=%d pos=%02d:%02d",
                current_disc_name,
                current_playback_status.disc_type,
                current_playback_status.is_playing,
                current_playback_status.audio_status,
                current_playback_status.current_track,
                current_playback_status.track_position_m,
                current_playback_status.track_position_s);
    }
}

// Function to refresh disc list from host
static esp_err_t refresh_disc_list(void) {
    if (!host_comm.initialized) {
        ESP_LOGW(TAG, "Host comm not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Clear current disc list
    menu_clear_items(&disc_menu);
    
    // Get disc count from host
    uint32_t disc_count = 0;
    esp_err_t ret = host_comm_get_disc_count(&host_comm, &disc_count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get disc count: %s", esp_err_to_name(ret));
        // Add fallback message
        menu_add_item(&disc_menu, "No discs available", MENU_ACTION_SELECT, NULL, NULL);
        return ret;
    }
    
    ESP_LOGI(TAG, "Found %lu discs on host", disc_count);
    
    if (disc_count == 0) {
        menu_add_item(&disc_menu, "No discs available", MENU_ACTION_SELECT, NULL, NULL);
        return ESP_OK;
    }

    // Get info for each disc
    for (uint32_t i = 0; i < disc_count && i < MENU_MAX_ITEMS; i++) {
        disc_info_t disc_info;
        ESP_LOGI(TAG, "Requesting disc info for index %lu", i);
        ret = host_comm_get_disc_info(&host_comm, i, &disc_info);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Disc %lu: %s (%.1f MB, %lu tracks)", i, disc_info.name, 
                    disc_info.size / 1000000.0f, disc_info.tracks);
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
    active_menu = &main_menu;
    ui_draw_menu(&display, &main_menu);
    vTaskDelete(NULL);
}

// Check and perform firmware update if available
static esp_err_t check_and_update_firmware(void) {
    ESP_LOGI(TAG, "Checking for firmware updates...");

    bool update_available = false;
    esp_err_t ret = ota_manager_check_update(&ota_manager, &update_available);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to check for updates: %s", esp_err_to_name(ret));
        return ret;
    }

    if (!update_available) {
        ESP_LOGI(TAG, "No firmware update available");
        return ESP_OK;
    }

    char version_str[14];
    ota_manager_format_version_string(version_str, ota_manager.firmware_info.version);
    ESP_LOGI(TAG, "Firmware update available! Version: %s", version_str);

    ret = ota_manager_start_update(&ota_manager);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start update: %s", esp_err_to_name(ret));
        return ret;
    }

    current_screen = SCREEN_FIRMWARE_UPDATE;

    char status_msg[64];
    snprintf(status_msg, sizeof(status_msg), "Found new fw: v%s", version_str);
    ui_draw_firmware_update(&display, status_msg, 0);
    vTaskDelay(pdMS_TO_TICKS(1500));

    xTaskCreate(firmware_update_task, "firmware_update", 8192, &ota_manager, 10, NULL);

    while (ota_manager_get_state(&ota_manager) != OTA_STATE_ERROR) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return ESP_ERR_INVALID_STATE;
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

    int initial_state = gpio_get_level(PIN_ACT_IN);
    if (initial_state) {
        led_set_color(COLOR_ORANGE);
        ESP_LOGI(TAG, "Initial PIN_ACT_IN state: HIGH (LED: Orange)");
    } else {
        led_set_color(COLOR_CYAN);
        ESP_LOGI(TAG, "Initial PIN_ACT_IN state: LOW (LED: Cyan)");
    }
    
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
    menu_init(&main_menu, 8);
    menu_set_items(&main_menu, main_menu_items,
                  sizeof(main_menu_items) / sizeof(main_menu_items[0]));

    menu_init(&disc_menu, 8);

    menu_init(&settings_menu, 8);
    menu_set_items(&settings_menu, settings_menu_items,
                  sizeof(settings_menu_items) / sizeof(settings_menu_items[0]));

    menu_init(&wifi_menu, 8);
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

        host_status_t test_status;
        ret = host_comm_get_status(&host_comm, &test_status);
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

            ret = ota_manager_init(&ota_manager, &host_comm);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to initialize OTA manager: %s", esp_err_to_name(ret));
            } else {
                ui_update_splash_progress(&display, "Check firmware...", 60);
                ret = check_and_update_firmware();
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "Firmware check completed");
                }
            }

            ESP_LOGI(TAG, "Fetching disc list at startup...");
            ui_update_splash_progress(&display, "Load disc list...", 70);
            esp_err_t disc_ret = refresh_disc_list();
            if (disc_ret == ESP_OK) {
                ESP_LOGI(TAG, "Disc list loaded successfully at startup");
            } else {
                ESP_LOGW(TAG, "Failed to load disc list at startup: %s", esp_err_to_name(disc_ret));
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
    active_menu = NULL;
    refresh_playback_status();
    ui_draw_status_screen(&display, current_disc_name, &current_playback_status, disc_name_changed);
    disc_name_changed = false;

    int initial_act_state = gpio_get_level(PIN_ACT_IN);
    led_set_color(initial_act_state ? COLOR_ORANGE : COLOR_CYAN);

    ESP_LOGI(TAG, "System initialized successfully");
    
    // Main loop can be used for other tasks or left empty
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
