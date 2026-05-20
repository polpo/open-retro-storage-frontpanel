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

#ifndef INTERFACE_COMMON_H
#define INTERFACE_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "host_comm.h"
#include "wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

// Common interface operations that both menu and web can use
typedef struct {
    host_comm_t *host_comm;
    wifi_manager_t *wifi_manager;
    uint16_t active_device_index;
} interface_context_t;

// System information structure
typedef struct {
    char main_firmware_version[20];
    char panel_firmware_version[20];
    const char *transport_name;
    bool host_connected;
    uint32_t free_memory;
    uint32_t uptime_seconds;
    wifi_manager_state_t wifi_state;
    char wifi_ip[16];
    char current_disc[64];
} system_info_t;

// Directory and image operations
esp_err_t interface_get_entry_list(interface_context_t *ctx, dir_entry_info_t *entries, size_t max_entries, size_t *entry_count);
esp_err_t interface_select_entry(interface_context_t *ctx, int32_t entry_index, uint16_t device_index);
esp_err_t interface_eject_image(interface_context_t *ctx, uint16_t device_index);
esp_err_t interface_get_current_image(interface_context_t *ctx, uint16_t device_index, char *image_name, size_t max_len);

// Device list
esp_err_t interface_get_device_list(interface_context_t *ctx, device_list_response_t *response, size_t max_size);

// WiFi operations
esp_err_t interface_wifi_scan(interface_context_t *ctx, wifi_ap_info_t *networks, size_t max_networks, size_t *found_networks);
esp_err_t interface_wifi_connect(interface_context_t *ctx, const char *ssid, const char *password);
esp_err_t interface_wifi_disconnect(interface_context_t *ctx);
esp_err_t interface_wifi_get_status(interface_context_t *ctx, wifi_manager_state_t *state, char *ip_address, size_t ip_max_len);

// System operations
esp_err_t interface_get_system_info(interface_context_t *ctx, system_info_t *info);

// Utility functions
const char* interface_wifi_state_string(wifi_manager_state_t state);
const char* interface_auth_mode_string(wifi_auth_mode_t auth_mode);

#ifdef __cplusplus
}
#endif

#endif // INTERFACE_COMMON_H