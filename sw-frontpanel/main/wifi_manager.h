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

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MANAGER_SSID_MAX_LEN 32
#define WIFI_MANAGER_PASSWORD_MAX_LEN 64

#ifdef CONFIG_PRODUCT_BLUESCSI
#define WIFI_MANAGER_AP_SSID "BlueSCSI-FrontPanel"
#define WIFI_MANAGER_MDNS_HOSTNAME "bluescsi"
#define WIFI_MANAGER_MDNS_INSTANCE "BlueSCSI Front Panel"
#define PRODUCT_NAME_FULL "BlueSCSI Front Panel"
#define PRODUCT_NAME_SHORT "BlueSCSI"
#define PRODUCT_LOGO_URL "/logo.svg"
#else
#define WIFI_MANAGER_AP_SSID "PicoIDE-FrontPanel"
#define WIFI_MANAGER_MDNS_HOSTNAME "picoide"
#define WIFI_MANAGER_MDNS_INSTANCE "PicoIDE Front Panel"
#define PRODUCT_NAME_FULL "PicoIDE Front Panel"
#define PRODUCT_NAME_SHORT "PicoIDE"
#define PRODUCT_LOGO_URL "/logo.svg"
#endif

#define WIFI_MANAGER_AP_PASSWORD "frontpanel123"
#define WIFI_MANAGER_AP_CHANNEL 1
#define WIFI_MANAGER_AP_MAX_CONNECTIONS 4
#define WIFI_MANAGER_CONNECTION_RETRY_MAX 5
#define WIFI_MANAGER_CONNECTION_RETRY_DELAY_MS 1000

typedef enum {
    WIFI_MANAGER_STATE_IDLE,
    WIFI_MANAGER_STATE_CONNECTING,
    WIFI_MANAGER_STATE_CONNECTED,
    WIFI_MANAGER_STATE_DISCONNECTED,
    WIFI_MANAGER_STATE_AP_MODE,
    WIFI_MANAGER_STATE_ERROR
} wifi_manager_state_t;

typedef struct {
    char ssid[WIFI_MANAGER_SSID_MAX_LEN];
    char password[WIFI_MANAGER_PASSWORD_MAX_LEN];
    bool has_password;
    int rssi;
    wifi_auth_mode_t auth_mode;
} wifi_ap_info_t;

typedef struct {
    char ssid[WIFI_MANAGER_SSID_MAX_LEN];
    char password[WIFI_MANAGER_PASSWORD_MAX_LEN];
    bool auto_connect;
    bool ap_mode_enabled;
    uint32_t connection_timeout_ms;
    uint8_t max_retry_attempts;
} wifi_manager_config_t;

typedef struct {
    wifi_manager_state_t state;
    wifi_manager_config_t config;
    bool initialized;
    bool station_connected;
    bool ap_active;
    bool pending_credential_save;  // Save credentials after successful DHCP
    uint8_t retry_count;
    esp_ip4_addr_t ip_addr;
    esp_ip4_addr_t ap_ip_addr;
    void (*on_connected)(esp_ip4_addr_t ip);
    void (*on_disconnected)(void);
    void (*on_ap_started)(esp_ip4_addr_t ip);
    void (*on_state_changed)(wifi_manager_state_t state);
} wifi_manager_t;

// Initialization and cleanup
esp_err_t wifi_manager_init(wifi_manager_t *manager);
esp_err_t wifi_manager_deinit(wifi_manager_t *manager);

// Configuration
esp_err_t wifi_manager_set_config(wifi_manager_t *manager, const wifi_manager_config_t *config);
esp_err_t wifi_manager_get_config(wifi_manager_t *manager, wifi_manager_config_t *config);
esp_err_t wifi_manager_save_config(wifi_manager_t *manager);
esp_err_t wifi_manager_load_config(wifi_manager_t *manager);
esp_err_t wifi_manager_clear_config(wifi_manager_t *manager);

// Connection management
esp_err_t wifi_manager_connect(wifi_manager_t *manager, const char *ssid, const char *password);
esp_err_t wifi_manager_connect_and_save(wifi_manager_t *manager, const char *ssid, const char *password);
esp_err_t wifi_manager_disconnect(wifi_manager_t *manager);
esp_err_t wifi_manager_start_ap(wifi_manager_t *manager);
esp_err_t wifi_manager_stop_ap(wifi_manager_t *manager);

// Status and information
wifi_manager_state_t wifi_manager_get_state(wifi_manager_t *manager);
bool wifi_manager_is_connected(wifi_manager_t *manager);
esp_err_t wifi_manager_get_ip_info(wifi_manager_t *manager, esp_ip4_addr_t *ip, esp_ip4_addr_t *netmask, esp_ip4_addr_t *gateway);
esp_err_t wifi_manager_scan_networks(wifi_manager_t *manager, wifi_ap_info_t *ap_list, size_t max_aps, size_t *found_aps);

// Incremental scan functions for streaming
esp_err_t wifi_manager_scan_start(wifi_manager_t *manager);
esp_err_t wifi_manager_scan_get_count(wifi_manager_t *manager, uint16_t *count);
esp_err_t wifi_manager_scan_get_ap(wifi_manager_t *manager, wifi_ap_info_t *ap_info);
esp_err_t wifi_manager_scan_cleanup(wifi_manager_t *manager);

// Event callbacks
esp_err_t wifi_manager_set_callbacks(wifi_manager_t *manager,
                                   void (*on_connected)(esp_ip4_addr_t ip),
                                   void (*on_disconnected)(void),
                                   void (*on_ap_started)(esp_ip4_addr_t ip),
                                   void (*on_state_changed)(wifi_manager_state_t state));

// mDNS functions
esp_err_t wifi_manager_start_mdns(wifi_manager_t *manager);
esp_err_t wifi_manager_stop_mdns(wifi_manager_t *manager);

// Utility functions
const char* wifi_manager_state_to_string(wifi_manager_state_t state);
const char* wifi_manager_auth_mode_to_string(wifi_auth_mode_t auth_mode);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H