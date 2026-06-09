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
#include "transport_config.h"
#include "panel_protocol_defs.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_rom_sys.h"   // esp_rom_delay_us (microsecond inter-phase delay)
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

// Read `len` bytes from the slave in a single I2C read transaction
// (START, address+R, ACKed reads, final NACK, STOP).
static esp_err_t transport_i2c_read_payload(transport_handle_t *handle,
                                            uint8_t *data, size_t len) {
    if (!handle || !handle->priv || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    transport_i2c_priv_t *priv = (transport_i2c_priv_t *)handle->priv;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (handle->config.device_addr << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(priv->port, cmd, pdMS_TO_TICKS(handle->config.timeout_ms));
    i2c_cmd_link_delete(cmd);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C payload read failed: %s", esp_err_to_name(ret));
    }
    return ret;
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
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (handle->config.device_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, (uint8_t*)&header, sizeof(header), true);

    // If write command with payload, append it to the same transaction
    if (!is_read && write_data && write_len > 0) {
        i2c_master_write(cmd, write_data, write_len, true);
    }

    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(priv->port, cmd, pdMS_TO_TICKS(handle->config.timeout_ms));
    i2c_cmd_link_delete(cmd);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Phase 1 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Phase 2: Read response (if read command)
    if (is_read && read_len > 0) {
#if I2C_INTER_PHASE_DELAY_US > 0
        // Small delay to let slave prepare response
        esp_rom_delay_us(I2C_INTER_PHASE_DELAY_US);
#endif
        ret = transport_i2c_read_payload(handle, read_data, read_len);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ret;
}

// Poll an async operation's status, mirroring transport_spi_poll_async_status().
// Issues POLL_OP_READY, reads the 3-byte status, and when the result is ready
// reads the result payload in a separate transaction.
esp_err_t transport_i2c_poll_async_status(transport_handle_t *handle,
                                          bool *ready, uint16_t *response_size,
                                          uint8_t *result_data, size_t max_result_size) {
    if (!handle || !ready || !response_size) {
        return ESP_ERR_INVALID_ARG;
    }

    panel_status_response_t status;
    esp_err_t ret = transport_i2c_two_phase_transaction(handle,
                                                        PANEL_CMD_POLL_OP_READY, PANEL_ARG_IGNORED,
                                                        NULL, 0,
                                                        (uint8_t*)&status, sizeof(status));
    if (ret != ESP_OK) {
        return ret;
    }
    if (status.ready_flag == PANEL_ASYNC_ERROR) {
        return ESP_FAIL;
    }

    *ready = (status.ready_flag == PANEL_ASYNC_READY);
    *response_size = status.response_size;

    // If ready and the caller wants the result, read it in a separate transaction
    if (*ready && *response_size > 0 && result_data && max_result_size > 0) {
#if I2C_INTER_PHASE_DELAY_US > 0
        // Delay to allow the main board to prepare the result data
        esp_rom_delay_us(I2C_INTER_PHASE_DELAY_US);
#endif
        size_t read_size = (*response_size <= max_result_size) ? *response_size : max_result_size;
        ret = transport_i2c_read_payload(handle, result_data, read_size);
    }

    return ret;
}

// I2C transport operations
const transport_ops_t transport_i2c_ops = {
    .init = transport_i2c_init,
    .deinit = transport_i2c_deinit,
    .two_phase_transaction = transport_i2c_two_phase_transaction,
    .poll_async_status = transport_i2c_poll_async_status,
    .get_name = transport_i2c_get_name,
};
