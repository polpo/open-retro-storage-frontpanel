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

#define WIFI_MANAGER_SSID_MAX_LEN 33  // 802.11 maximum of 32 bytes plus our NULL
#define WIFI_MANAGER_PASSWORD_MAX_LEN 64
// An mDNS hostname is a single DNS label: 63 characters max (RFC 1035) plus our NULL
#define WIFI_MANAGER_HOSTNAME_MAX_LEN 64

#ifdef CONFIG_PRODUCT_BLUESCSI
#define WIFI_MANAGER_AP_SSID_PREFIX "BlueSCSI-FrontPanel"
#define WIFI_MANAGER_MDNS_HOSTNAME_DEFAULT "bluescsi"
#define WIFI_MANAGER_MDNS_INSTANCE "BlueSCSI Front Panel"
#define PRODUCT_NAME_FULL "BlueSCSI Front Panel"
#define PRODUCT_NAME_SHORT "BlueSCSI"
#define PRODUCT_LOGO_URL "/logo.svg"
#else
#define WIFI_MANAGER_AP_SSID_PREFIX "PicoIDE-FrontPanel"
#define WIFI_MANAGER_MDNS_HOSTNAME_DEFAULT "picoide"
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
    wifi_manager_state_t state;

    // Persisted settings: each lives under its own NVS key instead of the old
    // legacy blob. Keeps the format compatible for upgrades/downgrades: adding
    // a new field brings in its default, and an old build only cares about old
    // fields it knows about.
    char ssid[WIFI_MANAGER_SSID_MAX_LEN];
    char password[WIFI_MANAGER_PASSWORD_MAX_LEN];
    bool auto_connect;
    bool ap_mode_enabled;
    uint32_t connection_timeout_ms;
    uint8_t max_retry_attempts;
    char ap_ssid[WIFI_MANAGER_SSID_MAX_LEN];
    char mdns_hostname[WIFI_MANAGER_HOSTNAME_MAX_LEN];

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
esp_err_t wifi_manager_save_settings(wifi_manager_t *manager);
esp_err_t wifi_manager_load_settings(wifi_manager_t *manager);
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

// Panel identity. The AP SSID defaults to the product prefix plus the last two
// bytes of the SoftAP MAC, so several panels are distinguishable out of the
// box; the mDNS hostname defaults to the bare product name. Both can be
// overridden and are persisted to NVS. Setting the hostname re-announces
// immediately; a new AP SSID applies the next time the AP starts.
const char *wifi_manager_get_ap_ssid(wifi_manager_t *manager);
const char *wifi_manager_get_mdns_hostname(wifi_manager_t *manager);
esp_err_t wifi_manager_set_ap_ssid(wifi_manager_t *manager, const char *ssid);
esp_err_t wifi_manager_set_mdns_hostname(wifi_manager_t *manager, const char *hostname);
void wifi_manager_default_ap_ssid(char *out, size_t len);
void wifi_manager_default_mdns_hostname(char *out, size_t len);
// Exposed so a caller setting both names can reject the pair up front rather
// than persisting the first and failing on the second.
bool wifi_manager_ap_ssid_is_valid(const char *ssid);
bool wifi_manager_mdns_hostname_is_valid(const char *hostname);

// Utility functions
const char* wifi_manager_state_to_string(wifi_manager_state_t state);
const char* wifi_manager_auth_mode_to_string(wifi_auth_mode_t auth_mode);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
