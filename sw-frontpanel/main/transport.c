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

#include "transport.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "transport";

// External transport operations
extern const transport_ops_t transport_i2c_ops;
extern const transport_ops_t transport_spi_ops;

// Factory function to get the appropriate transport implementation
const transport_ops_t* transport_get_ops(void) {
#ifdef TRANSPORT_USE_SPI
    ESP_LOGI(TAG, "Using SPI transport");
    return &transport_spi_ops;
#else
    ESP_LOGI(TAG, "Using I2C transport");
    return &transport_i2c_ops;
#endif
}

// Public API implementation
esp_err_t transport_init(transport_handle_t *handle, const transport_config_t *config) {
    if (!handle || !config) {
        return ESP_ERR_INVALID_ARG;
    }

    // Clear handle
    memset(handle, 0, sizeof(transport_handle_t));
    
    // Copy configuration
    memcpy(&handle->config, config, sizeof(transport_config_t));
    
    // Get appropriate transport operations
    handle->ops = transport_get_ops();
    
    if (!handle->ops) {
        ESP_LOGE(TAG, "Failed to get transport operations");
        return ESP_ERR_NOT_FOUND;
    }
    
    // Initialize the transport
    return handle->ops->init(handle);
}

esp_err_t transport_deinit(transport_handle_t *handle) {
    if (!handle || !handle->ops) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!handle->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    return handle->ops->deinit(handle);
}

const char* transport_get_name(transport_handle_t *handle) {
    if (!handle || !handle->ops) {
        return "Unknown";
    }
    
    return handle->ops->get_name();
}

// New protocol functions for two-phase transactions
esp_err_t transport_two_phase_transaction(transport_handle_t *handle,
                                          uint8_t command, uint16_t argument,
                                          const uint8_t *write_data, size_t write_len,
                                          uint8_t *read_data, size_t read_len) {
    if (!handle || !handle->ops) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!handle->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Route to transport-specific implementation
    return handle->ops->two_phase_transaction(handle, command, argument,
                                              write_data, write_len,
                                              read_data, read_len);
}

esp_err_t transport_poll_async_status(transport_handle_t *handle,
                                          bool *ready, uint16_t *response_size,
                                          uint8_t *result_data, size_t max_result_size) {
    if (!handle || !handle->ops) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!handle->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Route to transport-specific implementation
    return handle->ops->poll_async_status(handle, ready, response_size, result_data, max_result_size);
}

