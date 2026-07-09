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

#include "interface_common.h"
#include "ota_manager.h"
#include "fw_version.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "interface_common";

esp_err_t interface_get_entry_list(interface_context_t *ctx, dir_entry_info_t *entries, size_t max_entries, size_t *entry_count) {
    if (!ctx || !entries || !entry_count) {
        return ESP_ERR_INVALID_ARG;
    }

    *entry_count = 0;

    if (!ctx->host_comm || !ctx->host_comm->initialized) {
        ESP_LOGW(TAG, "Host communication not available");
        return ESP_ERR_INVALID_STATE;
    }

    // Get entry count from host
    uint32_t total_entry_count = 0;
    esp_err_t ret = host_comm_get_entry_count(ctx->host_comm, &total_entry_count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get entry count: %s", esp_err_to_name(ret));
        return ret;
    }

    if (total_entry_count == 0) {
        return ESP_OK; // No entries available
    }

    // Get info for each entry up to the maximum requested
    size_t entries_to_fetch = (total_entry_count < max_entries) ? total_entry_count : max_entries;
    for (size_t i = 0; i < entries_to_fetch; i++) {
        ret = host_comm_get_entry_info(ctx->host_comm, i, &entries[i]);
        if (ret == ESP_OK) {
            (*entry_count)++;
        } else {
            ESP_LOGW(TAG, "Failed to get info for entry %u: %s", i, esp_err_to_name(ret));
            // Create a fallback entry
            snprintf(entries[i].name, sizeof(entries[i].name), "Entry %u (error)", i);
            entries[i].entry_type = 1; // FILE
            (*entry_count)++;
        }
    }

    ESP_LOGI(TAG, "Retrieved info for %u entries", *entry_count);
    return ESP_OK;
}

esp_err_t interface_select_entry(interface_context_t *ctx, int32_t entry_index, uint16_t device_index) {
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ctx->host_comm || !ctx->host_comm->initialized) {
        ESP_LOGW(TAG, "Host communication not available");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = host_comm_select_entry(ctx->host_comm, entry_index, device_index);
    if (ret == ESP_OK) {
        if (entry_index == -1) {
            ESP_LOGI(TAG, "Navigated to parent directory");
        } else {
            ESP_LOGI(TAG, "Selected entry %ld", entry_index);
        }
    } else {
        ESP_LOGW(TAG, "Failed to select entry %ld: %s", entry_index, esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t interface_eject_image(interface_context_t *ctx, uint16_t device_index) {
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ctx->host_comm || !ctx->host_comm->initialized) {
        ESP_LOGW(TAG, "Host communication not available");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = host_comm_eject_image(ctx->host_comm, device_index);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Ejected image (device %u)", device_index);
    } else {
        ESP_LOGW(TAG, "Failed to eject image: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t interface_get_current_image(interface_context_t *ctx, uint16_t device_index, char *image_name, size_t max_len) {
    if (!ctx || !image_name || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ctx->host_comm || !ctx->host_comm->initialized) {
        strncpy(image_name, "No image loaded", max_len - 1);
        image_name[max_len - 1] = '\0';
        return ESP_OK;
    }

    loaded_image_status_t status;
    esp_err_t ret = host_comm_get_loaded_image_status(ctx->host_comm, device_index, &status);
    if (ret == ESP_OK && status.image_loaded) {
        strncpy(image_name, status.image_name, max_len - 1);
        image_name[max_len - 1] = '\0';
    } else {
        strncpy(image_name, "No image loaded", max_len - 1);
        image_name[max_len - 1] = '\0';
    }

    return ESP_OK;
}

esp_err_t interface_get_device_list(interface_context_t *ctx, device_list_response_t *response, size_t max_size) {
    if (!ctx || !response) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ctx->host_comm || !ctx->host_comm->initialized) {
        ESP_LOGW(TAG, "Host communication not available");
        return ESP_ERR_INVALID_STATE;
    }

    return host_comm_get_device_list(ctx->host_comm, response, max_size);
}

esp_err_t interface_wifi_scan(interface_context_t *ctx, wifi_ap_info_t *networks, size_t max_networks, size_t *found_networks) {
    if (!ctx || !networks || !found_networks) {
        return ESP_ERR_INVALID_ARG;
    }

    *found_networks = 0;

    if (!ctx->wifi_manager) {
        ESP_LOGW(TAG, "WiFi manager not available");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = wifi_manager_scan_networks(ctx->wifi_manager, networks, max_networks, found_networks);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi scan found %u networks", *found_networks);
    } else {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t interface_wifi_connect(interface_context_t *ctx, const char *ssid, const char *password) {
    if (!ctx || !ssid) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ctx->wifi_manager) {
        ESP_LOGW(TAG, "WiFi manager not available");
        return ESP_ERR_INVALID_STATE;
    }

    // Use connect_and_save so credentials persist after successful DHCP
    esp_err_t ret = wifi_manager_connect_and_save(ctx->wifi_manager, ssid, password);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Initiated WiFi connection to %s (will save on success)", ssid);
    } else {
        ESP_LOGW(TAG, "Failed to connect to WiFi %s: %s", ssid, esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t interface_wifi_disconnect(interface_context_t *ctx) {
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ctx->wifi_manager) {
        ESP_LOGW(TAG, "WiFi manager not available");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = wifi_manager_disconnect(ctx->wifi_manager);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi disconnected");
    } else {
        ESP_LOGW(TAG, "Failed to disconnect WiFi: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t interface_wifi_get_status(interface_context_t *ctx, wifi_manager_state_t *state, char *ip_address, size_t ip_max_len) {
    if (!ctx || !state) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ctx->wifi_manager) {
        ESP_LOGW(TAG, "WiFi manager not available");
        *state = WIFI_MANAGER_STATE_ERROR;
        if (ip_address && ip_max_len > 0) {
            ip_address[0] = '\0';
        }
        return ESP_ERR_INVALID_STATE;
    }

    *state = wifi_manager_get_state(ctx->wifi_manager);

    if (ip_address && ip_max_len > 0) {
        if (wifi_manager_is_connected(ctx->wifi_manager)) {
            esp_ip4_addr_t ip;
            esp_err_t ret = wifi_manager_get_ip_info(ctx->wifi_manager, &ip, NULL, NULL);
            if (ret == ESP_OK) {
                snprintf(ip_address, ip_max_len, IPSTR, IP2STR(&ip));
            } else {
                ip_address[0] = '\0';
            }
        } else {
            ip_address[0] = '\0';
        }
    }

    return ESP_OK;
}

esp_err_t interface_get_system_info(interface_context_t *ctx, system_info_t *info) {
    if (!ctx || !info) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(system_info_t));

    // Static system information
    info->free_memory = esp_get_free_heap_size();
    info->uptime_seconds = esp_log_timestamp() / 1000;

    // Firmware versions: the front panel runs its own image and the user-facing
    // "main" version is reported by the main board (empty if unreachable)
    fw_version_format_panel(info->panel_firmware_version,
                            sizeof(info->panel_firmware_version),
                            ota_manager_get_current_version());
    if (ctx->host_comm && ctx->host_comm->initialized) {
        rp2350_fw_status_t fw_status;
        if (host_comm_get_rp2350_fw_status(ctx->host_comm, &fw_status) == ESP_OK) {
            fw_version_format_mainboard(info->main_firmware_version,
                                        sizeof(info->main_firmware_version),
                                        fw_status.current_version);
        }
    }

    // Host communication info
    if (ctx->host_comm) {
        info->transport_name = host_comm_get_transport_name(ctx->host_comm);
        info->host_connected = ctx->host_comm->initialized;
    } else {
        info->transport_name = "None";
        info->host_connected = false;
    }

    // WiFi information
    if (ctx->wifi_manager) {
        info->wifi_state = wifi_manager_get_state(ctx->wifi_manager);
        if (wifi_manager_is_connected(ctx->wifi_manager)) {
            esp_ip4_addr_t ip;
            esp_err_t ret = wifi_manager_get_ip_info(ctx->wifi_manager, &ip, NULL, NULL);
            if (ret == ESP_OK) {
                snprintf(info->wifi_ip, sizeof(info->wifi_ip), IPSTR, IP2STR(&ip));
            }
        }
    } else {
        info->wifi_state = WIFI_MANAGER_STATE_ERROR;
    }

    // Current image information
    interface_get_current_image(ctx, ctx->active_device_index, info->current_disc, sizeof(info->current_disc));

    return ESP_OK;
}

const char* interface_wifi_state_string(wifi_manager_state_t state) {
    return wifi_manager_state_to_string(state);
}

const char* interface_auth_mode_string(wifi_auth_mode_t auth_mode) {
    return wifi_manager_auth_mode_to_string(auth_mode);
}
