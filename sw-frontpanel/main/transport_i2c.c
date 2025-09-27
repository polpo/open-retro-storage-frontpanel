#include "transport.h"
#include "panel_protocol_defs.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "transport_i2c";

#define I2C_PORT_NUM    I2C_NUM_0
#define I2C_TX_BUF_DISABLE 0
#define I2C_RX_BUF_DISABLE 0

typedef struct {
    i2c_port_t port;
} transport_i2c_priv_t;

static esp_err_t transport_i2c_init(transport_handle_t *handle) {
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    // Allocate private data
    transport_i2c_priv_t *priv = calloc(1, sizeof(transport_i2c_priv_t));
    if (!priv) {
        return ESP_ERR_NO_MEM;
    }
    
    priv->port = I2C_PORT_NUM;
    handle->priv = priv;

    // Configure I2C master
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = handle->config.sda_miso,
        .scl_io_num = handle->config.scl_clk,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = handle->config.clock_speed,
    };

    esp_err_t ret = i2c_param_config(priv->port, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure I2C: %s", esp_err_to_name(ret));
        free(priv);
        handle->priv = NULL;
        return ret;
    }

    ret = i2c_driver_install(priv->port, conf.mode, I2C_RX_BUF_DISABLE,
                             I2C_TX_BUF_DISABLE, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install I2C driver: %s", esp_err_to_name(ret));
        free(priv);
        handle->priv = NULL;
        return ret;
    }

    handle->initialized = true;
    ESP_LOGI(TAG, "I2C transport initialized on port %d (SDA: %d, SCL: %d, addr: 0x%02X)",
             priv->port, handle->config.sda_miso, handle->config.scl_clk, 
             handle->config.device_addr);
    
    return ESP_OK;
}

static esp_err_t transport_i2c_deinit(transport_handle_t *handle) {
    if (!handle || !handle->priv) {
        return ESP_ERR_INVALID_ARG;
    }

    transport_i2c_priv_t *priv = (transport_i2c_priv_t *)handle->priv;
    
    i2c_driver_delete(priv->port);
    free(priv);
    handle->priv = NULL;
    handle->initialized = false;
    
    ESP_LOGI(TAG, "I2C transport deinitialized");
    return ESP_OK;
}

static const char* transport_i2c_get_name(void) {
    return "I2C";
}

// High-level two-phase transaction function (matching SPI interface)
esp_err_t transport_i2c_two_phase_transaction(transport_handle_t *handle,
                                              uint8_t command, uint16_t argument,
                                              const uint8_t *write_data, size_t write_len,
                                              uint8_t *read_data, size_t read_len) {
    if (!handle || !handle->priv) {
        return ESP_ERR_INVALID_ARG;
    }

    transport_i2c_priv_t *priv = (transport_i2c_priv_t *)handle->priv;

    ESP_LOGI(TAG, "Two-phase transaction: cmd=0x%02x, arg=0x%02x", command, argument);

    // Build header
    panel_protocol_header_t header = {
        .command = command,
        .argument = argument,
        .payload_size = 0
    };

    // Determine payload size
    bool is_read = PANEL_CMD_IS_READ(command);
    if (is_read && read_len > 0) {
        header.payload_size = read_len;
    } else if (!is_read && write_data && write_len > 0) {
        header.payload_size = write_len;
    }

    // Phase 1: Send header (write transaction)
    ESP_LOGI(TAG, "Phase 1: Sending header (4 bytes)");

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (handle->config.device_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, (uint8_t*)&header, sizeof(header), true);

    // If write command with payload, append it to the same transaction
    if (!is_read && write_data && write_len > 0) {
        ESP_LOGI(TAG, "Appending write payload (%u bytes)", write_len);
        i2c_master_write(cmd, write_data, write_len, true);
    }

    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(priv->port, cmd, pdMS_TO_TICKS(handle->config.timeout_ms));
    i2c_cmd_link_delete(cmd);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Phase 1 failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Phase 1 complete");

    // Phase 2: Read response (if read command)
    if (is_read && read_len > 0) {
        // Small delay to let slave prepare response
        vTaskDelay(pdMS_TO_TICKS(1));

        ESP_LOGI(TAG, "Phase 2: Reading response (%u bytes)", read_len);

        cmd = i2c_cmd_link_create();
        if (!cmd) {
            return ESP_ERR_NO_MEM;
        }

        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (handle->config.device_addr << 1) | I2C_MASTER_READ, true);

        if (read_len > 1) {
            i2c_master_read(cmd, read_data, read_len - 1, I2C_MASTER_ACK);
        }
        i2c_master_read_byte(cmd, read_data + read_len - 1, I2C_MASTER_NACK);
        i2c_master_stop(cmd);

        ret = i2c_master_cmd_begin(priv->port, cmd, pdMS_TO_TICKS(handle->config.timeout_ms));
        i2c_cmd_link_delete(cmd);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Phase 2 failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGI(TAG, "Phase 2 complete");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, read_data, (read_len > 32) ? 32 : read_len, ESP_LOG_INFO);

        // Special handling for POLL_OP_READY: check if ready and do additional read
        if (command == PANEL_CMD_POLL_OP_READY && read_len >= 3) {
            uint8_t ready_flag = read_data[0];
            uint16_t result_size = read_data[1] | (read_data[2] << 8);

            ESP_LOGI(TAG, "POLL_OP_READY response: ready=%u, size=%u", ready_flag, result_size);

            if (ready_flag == 1 && result_size > 0) {
                // Result is ready - do immediate additional read for result data
                ESP_LOGI(TAG, "Phase 3: Additional read for result data (%u bytes)", result_size);

                // Check if caller provided enough buffer space
                uint8_t *result_buffer = read_data + 3; // Append after status response
                size_t max_result_size = read_len - 3;

                if (result_size <= max_result_size) {
                    // Small delay before next read
                    vTaskDelay(pdMS_TO_TICKS(1));

                    cmd = i2c_cmd_link_create();
                    if (!cmd) {
                        return ESP_ERR_NO_MEM;
                    }

                    i2c_master_start(cmd);
                    i2c_master_write_byte(cmd, (handle->config.device_addr << 1) | I2C_MASTER_READ, true);

                    if (result_size > 1) {
                        i2c_master_read(cmd, result_buffer, result_size - 1, I2C_MASTER_ACK);
                    }
                    i2c_master_read_byte(cmd, result_buffer + result_size - 1, I2C_MASTER_NACK);
                    i2c_master_stop(cmd);

                    ret = i2c_master_cmd_begin(priv->port, cmd, pdMS_TO_TICKS(handle->config.timeout_ms));
                    i2c_cmd_link_delete(cmd);

                    if (ret == ESP_OK) {
                        ESP_LOGI(TAG, "Additional read complete, result data:");
                        ESP_LOG_BUFFER_HEX_LEVEL(TAG, result_buffer, (result_size > 32) ? 32 : result_size, ESP_LOG_INFO);
                    } else {
                        ESP_LOGE(TAG, "Additional read failed: %s", esp_err_to_name(ret));
                    }
                } else {
                    ESP_LOGW(TAG, "Result size %u exceeds available buffer space %u", result_size, max_result_size);
                    ret = ESP_ERR_INVALID_SIZE;
                }
            }
        }
    }

    return ret;
}

// I2C transport operations
const transport_ops_t transport_i2c_ops = {
    .init = transport_i2c_init,
    .deinit = transport_i2c_deinit,
    .two_phase_transaction = transport_i2c_two_phase_transaction,
    .poll_async_status = NULL, // FIXME implement
    .get_name = transport_i2c_get_name,
};
