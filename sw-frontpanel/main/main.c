#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/spi_master.h"

#include "gpio_pins.h"
#include "display_manager.h"
#include "button_handler.h"
#include "menu_system.h"
#include "host_comm.h"  // Modular transport layer (I2C/SPI)
#include "transport_config.h" // Transport configuration
#include "ui_screens.h"
#include "led_driver.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "interface_common.h"
#include "esp_netif_ip_addr.h"

static const char *TAG = "main";

// Global instances
static display_manager_t display;
static menu_t main_menu;
static menu_t disc_menu;
static menu_t wifi_menu;
static menu_t settings_menu;
static host_comm_t host_comm;
static wifi_manager_t wifi_manager;
static web_server_t web_server;
static interface_context_t interface_ctx;
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
                            if (!disc_list_loaded && host_comm.initialized) {
                                ESP_LOGI(TAG, "Loading disc list...");
                                refresh_disc_list();
                            }
                            current_screen = SCREEN_DISC_LIST;
                            ui_draw_menu(&display, &disc_menu);
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
                            ui_draw_menu(&display, &settings_menu);
                        } else if (selected == 3) { // "System Info"
                            current_screen = SCREEN_INFO;
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
            
        case SCREEN_SETTINGS:
            switch (event->button_id) {
                case 0: // Up button (North)
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_up(&settings_menu);
                        ui_draw_menu(&display, &settings_menu);
                    }
                    break;
                case 2: // Down button (South)
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_down(&settings_menu);
                        ui_draw_menu(&display, &settings_menu);
                    }
                    break;
                case 1: // Select button (East)
                    if (event->type == BUTTON_EVENT_CLICK) {
                        uint32_t selected = menu_get_selected_index(&settings_menu);
                        if (selected == 0) { // "WiFi Setup"
                            current_screen = SCREEN_WIFI_MENU;
                            ui_draw_menu(&display, &wifi_menu);
                        } else if (selected == 1) { // "Web Interface"
                            // TODO: Implement web interface settings
                            ESP_LOGI(TAG, "Web interface settings not implemented yet");
                        } else if (selected == 2) { // "Display Settings"
                            // TODO: Implement display settings
                            ESP_LOGI(TAG, "Display settings not implemented yet");
                        } else if (selected == 3) { // "Back"
                            current_screen = SCREEN_MAIN_MENU;
                            ui_draw_menu(&display, &main_menu);
                        }
                    }
                    break;
                case 3: // Back button (West)
                    current_screen = SCREEN_MAIN_MENU;
                    ui_draw_menu(&display, &main_menu);
                    break;
            }
            break;

        case SCREEN_WIFI_MENU:
            switch (event->button_id) {
                case 0: // Up button (North)
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_up(&wifi_menu);
                        ui_draw_menu(&display, &wifi_menu);
                    }
                    break;
                case 2: // Down button (South)
                    if (event->type == BUTTON_EVENT_CLICK || event->type == BUTTON_EVENT_REPEAT) {
                        menu_navigate_down(&wifi_menu);
                        ui_draw_menu(&display, &wifi_menu);
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
                            ui_draw_menu(&display, &settings_menu);
                        }
                    }
                    break;
                case 3: // Back button (West)
                    current_screen = SCREEN_SETTINGS;
                    ui_draw_menu(&display, &settings_menu);
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
        .miso_io_num = PIN_SPI_MISO,    // Enable MISO for host communication
        .sclk_io_num = PIN_SPI_CLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = 4096,        // Allow larger transfers
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize shared SPI bus: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Shared SPI bus initialized (MISO: %d, MOSI: %d, CLK: %d)",
             PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SPI_CLK);

    // Initialize display (will use existing SPI bus)
    ret = display_manager_init(&display);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display: %s", esp_err_to_name(ret));
        return;
    }

    /*    
    // Initialize LED driver
    ret = led_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LED driver: %s", esp_err_to_name(ret));
        return;
    }
    
    // Rainbow demo for 10 seconds to indicate startup
    // led_demo_rainbow(0xffffffff);

    for(;;) {
        led_set_color(COLOR_RED);
        vTaskDelay(pdMS_TO_TICKS(250));
        led_set_color(COLOR_GREEN);
        vTaskDelay(pdMS_TO_TICKS(250));
        led_set_color(COLOR_BLUE);
        vTaskDelay(pdMS_TO_TICKS(250));
        led_set_color(COLOR_YELLOW);
        vTaskDelay(pdMS_TO_TICKS(250));
        led_set_color(COLOR_CYAN);
        vTaskDelay(pdMS_TO_TICKS(250));
        led_set_color(COLOR_MAGENTA);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    */
    
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
    nav_button_config.gpio = PIN_NAV_UP;
    button_handler_add_button(0, &nav_button_config); // Up (North)
    
    nav_button_config.gpio = PIN_NAV_DOWN;
    button_handler_add_button(2, &nav_button_config); // Down (South)
    
    // Add action buttons without repeat (select/back)
    action_button_config.gpio = PIN_NAV_RIGHT;
    button_handler_add_button(1, &action_button_config); // Select (East)
    
    action_button_config.gpio = PIN_NAV_LEFT;
    button_handler_add_button(3, &action_button_config); // Back (West)
    
    // Register button event handler
    button_handler_register_callback(handle_button_event);
    button_handler_start();
    
    // Initialize menus
    menu_init(&main_menu, 8);
    menu_set_items(&main_menu, main_menu_items,
                  sizeof(main_menu_items) / sizeof(main_menu_items[0]));

    menu_init(&disc_menu, 8);
    // Disc menu will be populated dynamically from host communication

    menu_init(&settings_menu, 8);
    menu_set_items(&settings_menu, settings_menu_items,
                  sizeof(settings_menu_items) / sizeof(settings_menu_items[0]));

    menu_init(&wifi_menu, 8);
    menu_set_items(&wifi_menu, wifi_menu_items,
                  sizeof(wifi_menu_items) / sizeof(wifi_menu_items[0]));
    
    // Initialize host communication (I2C or SPI based on compile-time config)
    transport_config_t transport_cfg = {
        .device_addr = HOST_DEVICE_ADDR,
        .sda_miso = PIN_SDA,          // I2C SDA or SPI MISO
        .scl_clk = PIN_SCL,           // I2C SCL or SPI CLK
        .cs = PIN_HOST_CS,            // SPI CS (ignored for I2C)
        .mosi = PIN_SPI_MOSI,         // SPI MOSI (ignored for I2C)
        .clock_speed = HOST_CLOCK_SPEED,
        .timeout_ms = HOST_TIMEOUT_MS,
    };
    
    ret = host_comm_init(&host_comm, &transport_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize host comm: %s", esp_err_to_name(ret));
        // Continue anyway - host might not be connected
    } else {
        ESP_LOGI(TAG, "Host comm initialized successfully using %s", 
                 host_comm_get_transport_name(&host_comm));
        
        // Manually fetch disc list at startup
        ESP_LOGI(TAG, "Fetching disc list at startup...");
        esp_err_t disc_ret = refresh_disc_list();
        if (disc_ret == ESP_OK) {
            ESP_LOGI(TAG, "Disc list loaded successfully at startup");
        } else {
            ESP_LOGW(TAG, "Failed to load disc list at startup: %s", esp_err_to_name(disc_ret));
        }
    }

    // Initialize WiFi manager
    ret = wifi_manager_init(&wifi_manager);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize WiFi manager: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "WiFi manager initialized successfully");

        // Try to load saved WiFi configuration and auto-connect
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

    // Initialize web server
    ret = web_server_init(&web_server, WEB_SERVER_PORT);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize web server: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Web server initialized successfully");
    }

    // Setup interface context for common operations
    interface_ctx.host_comm = &host_comm;
    interface_ctx.wifi_manager = &wifi_manager;

    // Connect web server to interface context
    web_server_set_interface_context(&web_server, &interface_ctx);

    // Start web server (will work on any interface that becomes available)
    ESP_LOGI(TAG, "Starting web server");
    ret = web_server_start(&web_server);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start web server: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Web server started successfully");
        ESP_LOGI(TAG, "Access web interface at: http://picoide.local (when WiFi connects)");
    }

    // Switch to main menu
    current_screen = SCREEN_MAIN_MENU;
    ui_draw_menu(&display, &main_menu);
    ui_draw_status_bar(&display, current_disc_name);
    
    // Create display update task
    xTaskCreate(display_update_task, "display_update", 4096, NULL, 5, NULL);
    
    // Create I2C communication task (commented out to reduce debug noise)
    // xTaskCreate(host_comm_task, "host_comm", 4096, NULL, 3, NULL);
    
    ESP_LOGI(TAG, "System initialized successfully");
    
    // Main loop can be used for other tasks or left empty
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
