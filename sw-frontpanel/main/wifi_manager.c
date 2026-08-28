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

#include "wifi_manager.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_mac.h"
#include "mdns.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

#define NVS_NAMESPACE "wifi_config"

#define NVS_KEY_STA_SSID "sta_ssid"
#define NVS_KEY_STA_PASS "sta_pass"
#define NVS_KEY_AUTO_CONN "auto_conn"
#define NVS_KEY_AP_ENABLED "ap_enabled"
#define NVS_KEY_CONN_TMO "conn_tmo"
#define NVS_KEY_MAX_RETRY "max_retry"
#define NVS_KEY_AP_SSID "ap_ssid"
#define NVS_KEY_MDNS_HOST "mdns_host"

// The single blob these settings used to be stored as, under one key.
// Frozen in time, only used to migrate from old to new settings
#define NVS_KEY_LEGACY_BLOB "wifi_config"
typedef struct {
    char ssid[32];
    char password[64];
    bool auto_connect;
    bool ap_mode_enabled;
    uint32_t connection_timeout_ms;
    uint8_t max_retry_attempts;
} legacy_wifi_config_t;

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static wifi_manager_t *s_manager = NULL;
static wifi_mode_t s_original_mode;  // Original mode before scanning

static esp_err_t wifi_manager_start_mdns(wifi_manager_t *manager);
static esp_err_t wifi_manager_stop_mdns(wifi_manager_t *manager);

void wifi_manager_default_ap_ssid(char *out, size_t len) {
    uint8_t mac[6] = {0};
    // Reads the efuse value directly, so this is valid before the radio starts.
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, len, "%s-%02X%02X", WIFI_MANAGER_AP_SSID_PREFIX, mac[4], mac[5]);
}

void wifi_manager_default_mdns_hostname(char *out, size_t len) {
    snprintf(out, len, "%s", WIFI_MANAGER_MDNS_HOSTNAME_DEFAULT);
}

bool wifi_manager_ap_ssid_is_valid(const char *ssid) {
    size_t len = strlen(ssid);
    return len > 0 && len < WIFI_MANAGER_SSID_MAX_LEN;
}

// Validate against RFC 1035: ASCII letters, digits and hyphens only, no leading
// or trailing hyphen
bool wifi_manager_mdns_hostname_is_valid(const char *hostname) {
    size_t len = strlen(hostname);
    if (len == 0 || len >= WIFI_MANAGER_HOSTNAME_MAX_LEN) return false;
    if (hostname[0] == '-' || hostname[len - 1] == '-') return false;
    for (const char *c = hostname; *c; c++) {
        bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                  (*c >= '0' && *c <= '9') || *c == '-';
        if (!ok) return false;
    }
    return true;
}

static void set_default_settings(wifi_manager_t *manager) {
    manager->ssid[0] = '\0';
    manager->password[0] = '\0';
    manager->auto_connect = false;
    manager->ap_mode_enabled = true;
    manager->connection_timeout_ms = 10000;
    manager->max_retry_attempts = WIFI_MANAGER_CONNECTION_RETRY_MAX;
    wifi_manager_default_ap_ssid(manager->ap_ssid, sizeof(manager->ap_ssid));
    wifi_manager_default_mdns_hostname(manager->mdns_hostname, sizeof(manager->mdns_hostname));
}

static bool import_legacy_settings(nvs_handle_t handle, wifi_manager_t *manager) {
    legacy_wifi_config_t legacy;
    size_t size = sizeof(legacy);

    if (nvs_get_blob(handle, NVS_KEY_LEGACY_BLOB, &legacy, &size) != ESP_OK ||
        size != sizeof(legacy)) {
        return false;
    }

    memcpy(manager->ssid, legacy.ssid, sizeof(legacy.ssid));
    manager->ssid[sizeof(legacy.ssid)] = '\0';
    memcpy(manager->password, legacy.password, sizeof(manager->password));
    manager->password[sizeof(manager->password) - 1] = '\0';
    manager->auto_connect = legacy.auto_connect;
    manager->ap_mode_enabled = legacy.ap_mode_enabled;
    manager->connection_timeout_ms = legacy.connection_timeout_ms;
    manager->max_retry_attempts = legacy.max_retry_attempts;
    // The names keep their defaults: the legacy blob had no such fields.

    ESP_LOGI(TAG, "Imported WiFi settings from the pre-split layout");
    return true;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data) {
    if (!s_manager) return;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi station started");
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "WiFi station connected");
                s_manager->station_connected = true;
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
                ESP_LOGW(TAG, "WiFi disconnected, reason: %d", event->reason);
                s_manager->station_connected = false;
                s_manager->state = WIFI_MANAGER_STATE_DISCONNECTED;

                if (s_manager->retry_count < s_manager->max_retry_attempts) {
                    s_manager->retry_count++;
                    ESP_LOGI(TAG, "Retrying connection (%d/%d)", s_manager->retry_count, s_manager->max_retry_attempts);
                    s_manager->state = WIFI_MANAGER_STATE_CONNECTING;
                    esp_wifi_connect();
                } else {
                    ESP_LOGE(TAG, "WiFi connection failed after %d attempts", s_manager->max_retry_attempts);
                    s_manager->state = WIFI_MANAGER_STATE_ERROR;
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }

                if (s_manager->on_disconnected) {
                    s_manager->on_disconnected();
                }
                if (s_manager->on_state_changed) {
                    s_manager->on_state_changed(s_manager->state);
                }
                break;
            }

            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "WiFi Access Point started");
                s_manager->ap_active = true;
                s_manager->state = WIFI_MANAGER_STATE_AP_MODE;

                if (s_manager->on_ap_started) {
                    s_manager->on_ap_started(s_manager->ap_ip_addr);
                }
                if (s_manager->on_state_changed) {
                    s_manager->on_state_changed(s_manager->state);
                }
                break;

            case WIFI_EVENT_AP_STOP:
                ESP_LOGI(TAG, "WiFi Access Point stopped");
                s_manager->ap_active = false;
                break;

            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
                ESP_LOGI(TAG, "Station %02x:%02x:%02x:%02x:%02x:%02x connected to AP",
                        event->mac[0], event->mac[1], event->mac[2],
                        event->mac[3], event->mac[4], event->mac[5]);
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
                ESP_LOGI(TAG, "Station %02x:%02x:%02x:%02x:%02x:%02x disconnected from AP",
                        event->mac[0], event->mac[1], event->mac[2],
                        event->mac[3], event->mac[4], event->mac[5]);
                break;
            }
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
            s_manager->ip_addr = event->ip_info.ip;
            s_manager->state = WIFI_MANAGER_STATE_CONNECTED;
            s_manager->retry_count = 0;
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

            // Save credentials if connect_and_save was used
            if (s_manager->pending_credential_save) {
                s_manager->auto_connect = true;
                esp_err_t save_ret = wifi_manager_save_settings(s_manager);
                if (save_ret == ESP_OK) {
                    ESP_LOGI(TAG, "WiFi credentials saved for auto-reconnect");
                } else {
                    ESP_LOGW(TAG, "Failed to save WiFi credentials: %s", esp_err_to_name(save_ret));
                }
                s_manager->pending_credential_save = false;
            }

            if (s_manager->on_connected) {
                s_manager->on_connected(s_manager->ip_addr);
            }
            if (s_manager->on_state_changed) {
                s_manager->on_state_changed(s_manager->state);
            }
        }
    }
}

esp_err_t wifi_manager_init(wifi_manager_t *manager) {
    if (!manager) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(manager, 0, sizeof(wifi_manager_t));
    s_manager = manager;

    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create event group
    s_wifi_event_group = xEventGroupCreate();

    // Create network interfaces
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // Set default AP IP address
    manager->ap_ip_addr.addr = esp_ip4addr_aton("192.168.4.1");

    manager->state = WIFI_MANAGER_STATE_IDLE;
    manager->initialized = true;

    // Fills in defaults for anything not stored. Must run before start_mdns()
    // which uses the hostname loaded by this
    wifi_manager_load_settings(manager);

    // Register the mdns responder. It hooks WIFI/IP_EVENT itself, and logs
    // its own failures
    wifi_manager_start_mdns(manager);

    ESP_LOGI(TAG, "WiFi manager initialized");

    return ESP_OK;
}

esp_err_t wifi_manager_deinit(wifi_manager_t *manager) {
    if (!manager || !manager->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_manager_stop_mdns(manager);

    // Stop WiFi
    esp_wifi_stop();
    esp_wifi_deinit();

    // Unregister event handlers
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler);

    // Cleanup
    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    s_manager = NULL;
    manager->initialized = false;

    ESP_LOGI(TAG, "WiFi manager deinitialized");
    return ESP_OK;
}

esp_err_t wifi_manager_save_settings(wifi_manager_t *manager) {
    if (!manager) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }

    // NVS skips the write when the stored value already matches, so setting
    // every key costs flash only for the ones that actually changed.
    nvs_set_str(handle, NVS_KEY_STA_SSID, manager->ssid);
    nvs_set_str(handle, NVS_KEY_STA_PASS, manager->password);
    nvs_set_u8(handle, NVS_KEY_AUTO_CONN, manager->auto_connect);
    nvs_set_u8(handle, NVS_KEY_AP_ENABLED, manager->ap_mode_enabled);
    nvs_set_u32(handle, NVS_KEY_CONN_TMO, manager->connection_timeout_ms);
    nvs_set_u8(handle, NVS_KEY_MAX_RETRY, manager->max_retry_attempts);
    nvs_set_str(handle, NVS_KEY_AP_SSID, manager->ap_ssid);
    nvs_set_str(handle, NVS_KEY_MDNS_HOST, manager->mdns_hostname);

    ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Settings saved");
    } else {
        ESP_LOGE(TAG, "Failed to save settings: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t wifi_manager_load_settings(wifi_manager_t *manager) {
    if (!manager) {
        return ESP_ERR_INVALID_ARG;
    }

    set_default_settings(manager);

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "No stored settings, using defaults");
        return ret;
    }

    size_t len = sizeof(manager->ssid);
    bool found = (nvs_get_str(handle, NVS_KEY_STA_SSID, manager->ssid, &len) == ESP_OK);

    len = sizeof(manager->password);
    found |= (nvs_get_str(handle, NVS_KEY_STA_PASS, manager->password, &len) == ESP_OK);

    uint8_t flag;
    if (nvs_get_u8(handle, NVS_KEY_AUTO_CONN, &flag) == ESP_OK) {
        manager->auto_connect = flag;
        found = true;
    }
    if (nvs_get_u8(handle, NVS_KEY_AP_ENABLED, &flag) == ESP_OK) {
        manager->ap_mode_enabled = flag;
        found = true;
    }

    found |= (nvs_get_u32(handle, NVS_KEY_CONN_TMO, &manager->connection_timeout_ms) == ESP_OK);
    found |= (nvs_get_u8(handle, NVS_KEY_MAX_RETRY, &manager->max_retry_attempts) == ESP_OK);

    len = sizeof(manager->ap_ssid);
    found |= (nvs_get_str(handle, NVS_KEY_AP_SSID, manager->ap_ssid, &len) == ESP_OK);

    len = sizeof(manager->mdns_hostname);
    found |= (nvs_get_str(handle, NVS_KEY_MDNS_HOST, manager->mdns_hostname, &len) == ESP_OK);

    // No key at all means this is either a fresh device or one still holding
    // the old single blob.
    bool imported = !found && import_legacy_settings(handle, manager);
    nvs_close(handle);

    if (imported) {
        // Write the migrated imported settings, but leave the old one alone
        // in case we want to downgrade
        wifi_manager_save_settings(manager);
    }

    return (found || imported) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t wifi_manager_clear_config(wifi_manager_t *manager) {
    if (!manager) {
        return ESP_ERR_INVALID_ARG;
    }

    set_default_settings(manager);

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for clearing: %s", esp_err_to_name(ret));
        return ESP_OK;
    }

    nvs_erase_key(handle, NVS_KEY_STA_SSID);
    nvs_erase_key(handle, NVS_KEY_STA_PASS);
    nvs_erase_key(handle, NVS_KEY_AUTO_CONN);
    nvs_erase_key(handle, NVS_KEY_AP_ENABLED);
    nvs_erase_key(handle, NVS_KEY_CONN_TMO);
    nvs_erase_key(handle, NVS_KEY_MAX_RETRY);
    nvs_erase_key(handle, NVS_KEY_AP_SSID);
    nvs_erase_key(handle, NVS_KEY_MDNS_HOST);
    // The old blob goes too, or the next load would import it straight back.
    nvs_erase_key(handle, NVS_KEY_LEGACY_BLOB);

    nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Settings cleared");
    return ESP_OK;
}

esp_err_t wifi_manager_connect(wifi_manager_t *manager, const char *ssid, const char *password) {
    if (!manager || !ssid) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(ssid) >= WIFI_MANAGER_SSID_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    if (password && strlen(password) >= WIFI_MANAGER_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    // Update configuration
    strncpy(manager->ssid, ssid, WIFI_MANAGER_SSID_MAX_LEN - 1);
    manager->ssid[WIFI_MANAGER_SSID_MAX_LEN - 1] = '\0';

    if (password) {
        strncpy(manager->password, password, WIFI_MANAGER_PASSWORD_MAX_LEN - 1);
        manager->password[WIFI_MANAGER_PASSWORD_MAX_LEN - 1] = '\0';
    } else {
        manager->password[0] = '\0';
    }

    // Configure WiFi
    wifi_config_t wifi_config = {0};
    // sta.ssid is exactly 32 bytes and need not be NUL-terminated, so a
    // full-length SSID is copied as raw bytes rather than as a string.
    memcpy(wifi_config.sta.ssid, manager->ssid,
           strnlen(manager->ssid, sizeof(wifi_config.sta.ssid)));
    if (password) {
        strcpy((char*)wifi_config.sta.password, manager->password);
    }
    wifi_config.sta.threshold.authmode = (password && strlen(password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    manager->state = WIFI_MANAGER_STATE_CONNECTING;
    manager->retry_count = 0;

    if (manager->on_state_changed) {
        manager->on_state_changed(manager->state);
    }

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid);

    return esp_wifi_connect();
}

esp_err_t wifi_manager_connect_and_save(wifi_manager_t *manager, const char *ssid, const char *password) {
    if (!manager || !ssid) {
        return ESP_ERR_INVALID_ARG;
    }

    // Set flag to save credentials after successful DHCP
    manager->pending_credential_save = true;

    // Use regular connect
    return wifi_manager_connect(manager, ssid, password);
}

esp_err_t wifi_manager_disconnect(wifi_manager_t *manager) {
    if (!manager) {
        return ESP_ERR_INVALID_ARG;
    }

    manager->state = WIFI_MANAGER_STATE_IDLE;
    manager->station_connected = false;

    if (manager->on_state_changed) {
        manager->on_state_changed(manager->state);
    }

    return esp_wifi_disconnect();
}

esp_err_t wifi_manager_start_ap(wifi_manager_t *manager) {
    if (!manager) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_ap_config = {
        .ap = {
            .channel = WIFI_MANAGER_AP_CHANNEL,
            .password = WIFI_MANAGER_AP_PASSWORD,
            .max_connection = WIFI_MANAGER_AP_MAX_CONNECTIONS,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    // ap.ssid is 32 bytes with no terminator, read ssid_len bytes
    wifi_ap_config.ap.ssid_len = strnlen(manager->ap_ssid, sizeof(wifi_ap_config.ap.ssid));
    memcpy(wifi_ap_config.ap.ssid, manager->ap_ssid, wifi_ap_config.ap.ssid_len);

    if (strlen(WIFI_MANAGER_AP_PASSWORD) == 0) {
        wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started with SSID: %s", manager->ap_ssid);

    return ESP_OK;
}

esp_err_t wifi_manager_stop_ap(wifi_manager_t *manager) {
    if (!manager) {
        return ESP_ERR_INVALID_ARG;
    }

    manager->ap_active = false;
    return esp_wifi_stop();
}

wifi_manager_state_t wifi_manager_get_state(wifi_manager_t *manager) {
    if (!manager) {
        return WIFI_MANAGER_STATE_ERROR;
    }
    return manager->state;
}

bool wifi_manager_is_connected(wifi_manager_t *manager) {
    if (!manager) {
        return false;
    }
    return manager->station_connected && (manager->state == WIFI_MANAGER_STATE_CONNECTED);
}

esp_err_t wifi_manager_get_ip_info(wifi_manager_t *manager, esp_ip4_addr_t *ip, esp_ip4_addr_t *netmask, esp_ip4_addr_t *gateway) {
    if (!manager || !ip) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!wifi_manager_is_connected(manager)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(s_sta_netif, &ip_info);
    if (ret == ESP_OK) {
        *ip = ip_info.ip;
        if (netmask) *netmask = ip_info.netmask;
        if (gateway) *gateway = ip_info.gw;
    }

    return ret;
}

esp_err_t wifi_manager_scan_networks(wifi_manager_t *manager, wifi_ap_info_t *ap_list, size_t max_aps, size_t *found_aps) {
    if (!manager || !ap_list || !found_aps) {
        return ESP_ERR_INVALID_ARG;
    }

    *found_aps = 0;

    // Start scan
    esp_err_t ret = esp_wifi_scan_start(NULL, true);
    if (ret != ESP_OK) {
        return ret;
    }

    // Get scan results
    uint16_t number = max_aps;
    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * max_aps);
    if (!ap_records) {
        return ESP_ERR_NO_MEM;
    }

    ret = esp_wifi_scan_get_ap_records(&number, ap_records);
    if (ret != ESP_OK) {
        free(ap_records);
        return ret;
    }

    // Convert to our format
    for (int i = 0; i < number && i < max_aps; i++) {
        strncpy(ap_list[i].ssid, (char*)ap_records[i].ssid, WIFI_MANAGER_SSID_MAX_LEN - 1);
        ap_list[i].ssid[WIFI_MANAGER_SSID_MAX_LEN - 1] = '\0';
        ap_list[i].rssi = ap_records[i].rssi;
        ap_list[i].auth_mode = ap_records[i].authmode;
        ap_list[i].has_password = (ap_records[i].authmode != WIFI_AUTH_OPEN);
    }

    *found_aps = number;
    free(ap_records);

    ESP_LOGI(TAG, "WiFi scan completed, found %d networks", number);

    return ESP_OK;
}

esp_err_t wifi_manager_scan_start(wifi_manager_t *manager) {
    if (!manager) {
        return ESP_ERR_INVALID_ARG;
    }

    // Get current WiFi mode
    wifi_mode_t current_mode;
    esp_err_t ret = esp_wifi_get_mode(&current_mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get WiFi mode: %s", esp_err_to_name(ret));
        return ret;
    }

    // Save original mode before any changes
    s_original_mode = current_mode;

    // If in pure AP mode, temporarily switch to STA+AP mode for scanning
    if (current_mode == WIFI_MODE_AP) {
        ESP_LOGI(TAG, "Switching from AP to STA+AP mode for scanning");
        ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set WiFi mode to STA+AP: %s", esp_err_to_name(ret));
            return ret;
        }

        // Give WiFi time to stabilize after mode change
        vTaskDelay(pdMS_TO_TICKS(100));
    } else if (current_mode == WIFI_MODE_NULL) {
        // WiFi not started, start it in STA mode for scanning
        ESP_LOGI(TAG, "Starting WiFi in STA mode for scanning");
        ret = esp_wifi_set_mode(WIFI_MODE_STA);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(ret));
            return ret;
        }
        ret = esp_wifi_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(ret));
            return ret;
        }

        // Give WiFi time to start up
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Configure scan parameters for better results
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 100,
                .max = 300
            }
        }
    };

    // Start scan (blocking mode)
    ESP_LOGI(TAG, "Starting WiFi scan...");
    ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan start failed: %s", esp_err_to_name(ret));
    } else {
        // Log how many APs were found
        uint16_t ap_count = 0;
        esp_err_t count_ret = esp_wifi_scan_get_ap_num(&ap_count);
        if (count_ret == ESP_OK) {
            ESP_LOGI(TAG, "WiFi scan completed, found %d APs", ap_count);
        }
    }

    // Don't restore mode here - we need to keep the scan results available
    // Mode will be restored after cleanup

    return ret;
}

esp_err_t wifi_manager_scan_get_count(wifi_manager_t *manager, uint16_t *count) {
    if (!manager || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = esp_wifi_scan_get_ap_num(count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP count: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t wifi_manager_scan_get_ap(wifi_manager_t *manager, wifi_ap_info_t *ap_info) {
    if (!manager || !ap_info) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_ap_record_t ap_record;
    esp_err_t ret = esp_wifi_scan_get_ap_record(&ap_record);

    if (ret == ESP_OK) {
        // Convert to our format
        strncpy(ap_info->ssid, (char*)ap_record.ssid, WIFI_MANAGER_SSID_MAX_LEN - 1);
        ap_info->ssid[WIFI_MANAGER_SSID_MAX_LEN - 1] = '\0';
        ap_info->rssi = ap_record.rssi;
        ap_info->auth_mode = ap_record.authmode;
        ap_info->has_password = (ap_record.authmode != WIFI_AUTH_OPEN);
        // Clear password field for scanned networks
        memset(ap_info->password, 0, WIFI_MANAGER_PASSWORD_MAX_LEN);
    }

    return ret;
}

esp_err_t wifi_manager_scan_cleanup(wifi_manager_t *manager) {
    if (!manager) {
        return ESP_ERR_INVALID_ARG;
    }

    // Clear any remaining AP records that weren't consumed
    esp_wifi_clear_ap_list();

    // Check if mode was changed and restore if needed
    wifi_mode_t current_mode;
    esp_err_t ret = esp_wifi_get_mode(&current_mode);

    if (ret == ESP_OK && current_mode != s_original_mode) {
        ESP_LOGI(TAG, "Restoring WiFi mode from %d to %d after scan", current_mode, s_original_mode);
        ret = esp_wifi_set_mode(s_original_mode);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to restore WiFi mode: %s", esp_err_to_name(ret));
        }
    }

    ESP_LOGI(TAG, "WiFi scan cleanup completed");
    return ESP_OK;
}

esp_err_t wifi_manager_set_callbacks(wifi_manager_t *manager,
                                   void (*on_connected)(esp_ip4_addr_t ip),
                                   void (*on_disconnected)(void),
                                   void (*on_ap_started)(esp_ip4_addr_t ip),
                                   void (*on_state_changed)(wifi_manager_state_t state)) {
    if (!manager) {
        return ESP_ERR_INVALID_ARG;
    }

    manager->on_connected = on_connected;
    manager->on_disconnected = on_disconnected;
    manager->on_ap_started = on_ap_started;
    manager->on_state_changed = on_state_changed;

    return ESP_OK;
}

const char* wifi_manager_state_to_string(wifi_manager_state_t state) {
    switch (state) {
        case WIFI_MANAGER_STATE_IDLE: return "Idle";
        case WIFI_MANAGER_STATE_CONNECTING: return "Connecting";
        case WIFI_MANAGER_STATE_CONNECTED: return "Connected";
        case WIFI_MANAGER_STATE_DISCONNECTED: return "Disconnected";
        case WIFI_MANAGER_STATE_AP_MODE: return "Access Point";
        case WIFI_MANAGER_STATE_ERROR: return "Error";
        default: return "Unknown";
    }
}

const char* wifi_manager_auth_mode_to_string(wifi_auth_mode_t auth_mode) {
    switch (auth_mode) {
        case WIFI_AUTH_OPEN: return "Open";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        default: return "Unknown";
    }
}

static esp_err_t wifi_manager_start_mdns(wifi_manager_t *manager) {
    if (!manager) {
        return ESP_ERR_INVALID_ARG;
    }

    // Initialize mDNS
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize mDNS: %s", esp_err_to_name(ret));
        return ret;
    }

    // Set mDNS hostname
    ret = mdns_hostname_set(manager->mdns_hostname);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set mDNS hostname: %s", esp_err_to_name(ret));
        mdns_free();
        return ret;
    }

    // Set default mDNS instance name
    ret = mdns_instance_name_set(WIFI_MANAGER_MDNS_INSTANCE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set mDNS instance name: %s", esp_err_to_name(ret));
        mdns_free();
        return ret;
    }

    // Add HTTP service
    ret = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add mDNS HTTP service: %s", esp_err_to_name(ret));
        mdns_free();
        return ret;
    }

    // Add service instance name
    ret = mdns_service_instance_name_set("_http", "_tcp", PRODUCT_NAME_FULL " Web Interface");
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set mDNS service instance name: %s", esp_err_to_name(ret));
        // Don't fail for this, it's optional
    }

    ESP_LOGI(TAG, "mDNS started - device available at http://%s.local",
             manager->mdns_hostname);
    return ESP_OK;
}

static esp_err_t wifi_manager_stop_mdns(wifi_manager_t *manager) {
    if (!manager) {
        return ESP_ERR_INVALID_ARG;
    }

    mdns_free();
    ESP_LOGI(TAG, "mDNS stopped");
    return ESP_OK;
}

const char *wifi_manager_get_ap_ssid(wifi_manager_t *manager) {
    return manager ? manager->ap_ssid : "";
}

const char *wifi_manager_get_mdns_hostname(wifi_manager_t *manager) {
    return manager ? manager->mdns_hostname : "";
}

esp_err_t wifi_manager_set_ap_ssid(wifi_manager_t *manager, const char *ssid) {
    if (!manager || !ssid || !wifi_manager_ap_ssid_is_valid(ssid)) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(manager->ap_ssid, sizeof(manager->ap_ssid), "%s", ssid);
    ESP_LOGI(TAG, "AP SSID set to %s (applies on next AP start)", manager->ap_ssid);
    // Deliberately not restarting a running AP: the client that just asked for
    // the change is most likely connected over it.
    return wifi_manager_save_settings(manager);
}

esp_err_t wifi_manager_set_mdns_hostname(wifi_manager_t *manager, const char *hostname) {
    if (!manager || !hostname || !wifi_manager_mdns_hostname_is_valid(hostname)) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(manager->mdns_hostname, sizeof(manager->mdns_hostname),
             "%s", hostname);

    // Cheap to apply live: this re-probes and announces the new name without
    // tearing the responder down, so clients pick it up within a second.
    esp_err_t ret = mdns_hostname_set(manager->mdns_hostname);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to apply mDNS hostname: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "mDNS hostname set - device available at http://%s.local",
                 manager->mdns_hostname);
    }

    return wifi_manager_save_settings(manager);
}
