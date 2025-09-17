#include "interface_common.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "interface_common";

esp_err_t interface_get_disc_list(interface_context_t *ctx, disc_info_t *discs, size_t max_discs, size_t *disc_count) {
    if (!ctx || !discs || !disc_count) {
        return ESP_ERR_INVALID_ARG;
    }

    *disc_count = 0;

    if (!ctx->host_comm || !ctx->host_comm->initialized) {
        ESP_LOGW(TAG, "Host communication not available");
        return ESP_ERR_INVALID_STATE;
    }

    // Get disc count from host
    uint32_t total_disc_count = 0;
    esp_err_t ret = host_comm_get_disc_count(ctx->host_comm, &total_disc_count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get disc count: %s", esp_err_to_name(ret));
        return ret;
    }

    if (total_disc_count == 0) {
        return ESP_OK; // No discs available
    }

    // Get info for each disc up to the maximum requested
    size_t discs_to_fetch = (total_disc_count < max_discs) ? total_disc_count : max_discs;
    for (size_t i = 0; i < discs_to_fetch; i++) {
        ret = host_comm_get_disc_info(ctx->host_comm, i, &discs[i]);
        if (ret == ESP_OK) {
            (*disc_count)++;
        } else {
            ESP_LOGW(TAG, "Failed to get info for disc %zu: %s", i, esp_err_to_name(ret));
            // Create a fallback entry
            snprintf(discs[i].name, sizeof(discs[i].name), "Disc %zu (error)", i);
            discs[i].size = 0;
            discs[i].tracks = 0;
            (*disc_count)++;
        }
    }

    ESP_LOGI(TAG, "Retrieved info for %zu discs", *disc_count);
    return ESP_OK;
}

esp_err_t interface_select_disc(interface_context_t *ctx, uint32_t disc_index) {
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ctx->host_comm || !ctx->host_comm->initialized) {
        ESP_LOGW(TAG, "Host communication not available");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = host_comm_select_disc(ctx->host_comm, disc_index);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Selected disc %lu", disc_index);
    } else {
        ESP_LOGW(TAG, "Failed to select disc %lu: %s", disc_index, esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t interface_eject_disc(interface_context_t *ctx) {
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ctx->host_comm || !ctx->host_comm->initialized) {
        ESP_LOGW(TAG, "Host communication not available");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = host_comm_eject_disc(ctx->host_comm);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Ejected disc");
    } else {
        ESP_LOGW(TAG, "Failed to eject disc: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t interface_get_current_disc(interface_context_t *ctx, char *disc_name, size_t max_len) {
    if (!ctx || !disc_name || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // TODO: Implement current disc tracking
    // For now, return a placeholder
    strncpy(disc_name, "No disc loaded", max_len - 1);
    disc_name[max_len - 1] = '\0';

    return ESP_OK;
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
        ESP_LOGI(TAG, "WiFi scan found %zu networks", *found_networks);
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

    esp_err_t ret = wifi_manager_connect(ctx->wifi_manager, ssid, password);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Initiated WiFi connection to %s", ssid);
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
    info->firmware_version = "v0.1.0";
    info->hardware_name = "ESP32-C3";
    info->free_memory = esp_get_free_heap_size();
    info->uptime_seconds = esp_log_timestamp() / 1000;

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

    // Current disc information
    interface_get_current_disc(ctx, info->current_disc, sizeof(info->current_disc));

    return ESP_OK;
}

const char* interface_wifi_state_string(wifi_manager_state_t state) {
    return wifi_manager_state_to_string(state);
}

const char* interface_auth_mode_string(wifi_auth_mode_t auth_mode) {
    return wifi_manager_auth_mode_to_string(auth_mode);
}