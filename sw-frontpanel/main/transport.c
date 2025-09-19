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

esp_err_t transport_write(transport_handle_t *handle, const uint8_t *data, size_t len) {
    if (!handle || !handle->ops) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!handle->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    return handle->ops->write(handle, data, len);
}

esp_err_t transport_read(transport_handle_t *handle, uint8_t *data, size_t len) {
    if (!handle || !handle->ops) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!handle->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    return handle->ops->read(handle, data, len);
}

esp_err_t transport_write_then_read(transport_handle_t *handle,
                                     const uint8_t *write_data, size_t write_len,
                                     uint8_t *read_data, size_t read_len) {
    if (!handle || !handle->ops) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!handle->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    return handle->ops->write_then_read(handle, write_data, write_len, read_data, read_len);
}

const char* transport_get_name(transport_handle_t *handle) {
    if (!handle || !handle->ops) {
        return "Unknown";
    }

    return handle->ops->get_name();
}

// New protocol functions for two-phase transactions
esp_err_t transport_two_phase_transaction(transport_handle_t *handle,
                                          uint8_t command, uint8_t argument,
                                          const uint8_t *write_data, size_t write_len,
                                          uint8_t *read_data, size_t read_len) {
    if (!handle || !handle->ops) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!handle->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Route to transport-specific implementation
#ifdef TRANSPORT_USE_SPI
    return transport_spi_two_phase_transaction(handle, command, argument,
                                               write_data, write_len,
                                               read_data, read_len);
#else
    return transport_i2c_two_phase_transaction(handle, command, argument,
                                               write_data, write_len,
                                               read_data, read_len);
#endif
}