#include "i2c_slave_comm.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "i2c_slave_comm";

#define I2C_MASTER_FREQ_HZ 400000
#define I2C_MASTER_TX_BUF_DISABLE 0
#define I2C_MASTER_RX_BUF_DISABLE 0

// Note: This module assumes the I2C bus is already initialized by the display driver
// It will use the existing I2C driver to communicate with the slave device

esp_err_t i2c_slave_comm_init(i2c_slave_comm_t *comm, i2c_port_t port,
                              uint8_t slave_addr, gpio_num_t sda, gpio_num_t scl) {
    if (!comm) {
        return ESP_ERR_INVALID_ARG;
    }

    comm->port = port;
    comm->slave_addr = slave_addr;
    comm->sda_pin = sda;
    comm->scl_pin = scl;
    comm->clk_speed = I2C_MASTER_FREQ_HZ;
    comm->initialized = true;

    // Don't initialize I2C here - assume it's already initialized by display driver
    // Don't try to communicate yet - the I2C bus might still be busy with display init
    
    ESP_LOGI(TAG, "I2C slave comm initialized for slave addr 0x%02X on existing bus", slave_addr);
    
    return ESP_OK;
}

esp_err_t i2c_slave_comm_deinit(i2c_slave_comm_t *comm) {
    if (!comm || !comm->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Don't delete the I2C driver - it's managed by the display driver
    comm->initialized = false;
    ESP_LOGI(TAG, "I2C slave comm deinitialized");
    
    return ESP_OK;
}

esp_err_t i2c_slave_send_command(i2c_slave_comm_t *comm, i2c_command_t cmd,
                                 const uint8_t *data, size_t data_len,
                                 uint8_t *response, size_t *response_len) {
    if (!comm || !comm->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "i2c_slave_send_command: cmd=0x%02x, data_len=%u", cmd, data_len);
    if (data && data_len > 0) {
        ESP_LOGI(TAG, "i2c_slave_send_command: data=[0x%02x,0x%02x,0x%02x,0x%02x]", 
                 data[0], data_len > 1 ? data[1] : 0, data_len > 2 ? data[2] : 0, data_len > 3 ? data[3] : 0);
    }

    // Create I2C command link
    i2c_cmd_handle_t cmd_handle = i2c_cmd_link_create();
    if (cmd_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create I2C command link");
        return ESP_ERR_NO_MEM;
    }
    
    // Start condition
    i2c_master_start(cmd_handle);
    
    // Send slave address with write bit
    i2c_master_write_byte(cmd_handle, (comm->slave_addr << 1) | I2C_MASTER_WRITE, true);
    
    // Send command
    i2c_master_write_byte(cmd_handle, cmd, true);
    
    // For commands that need payload, send payload length first
    if (cmd == I2C_CMD_SELECT_DISC || cmd == I2C_CMD_GET_DISC_INFO) {
        // These commands have fixed 4-byte payload (disc index)
        i2c_master_write_byte(cmd_handle, 4, true);
        if (data && data_len > 0) {
            i2c_master_write(cmd_handle, data, data_len, true);
        }
    } else if (data && data_len > 0) {
        // For future extensible commands, send payload length then data
        i2c_master_write_byte(cmd_handle, (uint8_t)data_len, true);
        i2c_master_write(cmd_handle, data, data_len, true);
    }
    // Commands without payload (GET_STATUS, GET_DISC_COUNT, EJECT_DISC, RESET) 
    // don't send payload length
    
    // If we need to read a response, do it in the same transaction
    if (response && response_len && *response_len > 0) {
        // Repeated start condition (don't stop, continue to read)
        i2c_master_start(cmd_handle);
        
        // Send slave address with read bit
        i2c_master_write_byte(cmd_handle, (comm->slave_addr << 1) | I2C_MASTER_READ, true);
        
        ESP_LOGI(TAG, "i2c_slave_send_command: response_len: %u", *response_len);
        // Read response
        if (*response_len > 1) {
            ESP_LOGI(TAG, "Adding i2c_master_read for %u bytes with ACK", *response_len - 1);
            i2c_master_read(cmd_handle, response, *response_len - 1, I2C_MASTER_ACK);
        }
        ESP_LOGI(TAG, "Adding i2c_master_read_byte for final byte with NACK");
        i2c_master_read_byte(cmd_handle, response + (*response_len - 1), I2C_MASTER_NACK);
    }
    
    // Stop condition (end of transaction)
    i2c_master_stop(cmd_handle);
    
    // Execute the entire transaction
    esp_err_t ret = i2c_master_cmd_begin(comm->port, cmd_handle, pdMS_TO_TICKS(I2C_SLAVE_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd_handle);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to execute I2C transaction for command 0x%02X to slave 0x%02X: %s", 
                 cmd, comm->slave_addr, esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "i2c_slave_send_command: transaction completed successfully for cmd=0x%02x", cmd);
    return ESP_OK;
}

esp_err_t i2c_slave_get_status(i2c_slave_comm_t *comm, i2c_status_t *status) {
    if (!comm || !status) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t response;
    size_t response_len = 1;
    
    esp_err_t ret = i2c_slave_send_command(comm, I2C_CMD_GET_STATUS, NULL, 0,
                                           &response, &response_len);
    if (ret == ESP_OK) {
        *status = (i2c_status_t)response;
    }
    
    return ret;
}

esp_err_t i2c_slave_get_disc_count(i2c_slave_comm_t *comm, uint32_t *count) {
    if (!comm || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t response[4];
    size_t response_len = sizeof(response);
    
    esp_err_t ret = i2c_slave_send_command(comm, I2C_CMD_GET_DISC_COUNT, NULL, 0,
                                           response, &response_len);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "GET_DISC_COUNT response: [0x%02x, 0x%02x, 0x%02x, 0x%02x]", 
                 response[0], response[1], response[2], response[3]);
        *count = (response[0] << 24) | (response[1] << 16) | 
                (response[2] << 8) | response[3];
        ESP_LOGI(TAG, "Parsed count: %lu", *count);
    }
    
    return ret;
}

esp_err_t i2c_slave_get_disc_list(i2c_slave_comm_t *comm, disc_info_t *discs,
                                  uint32_t max_discs, uint32_t *actual_count) {
    if (!comm || !discs || !actual_count) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // First get the count
    uint32_t count;
    esp_err_t ret = i2c_slave_get_disc_count(comm, &count);
    if (ret != ESP_OK) {
        return ret;
    }
    
    *actual_count = (count < max_discs) ? count : max_discs;
    
    // Get info for each disc
    for (uint32_t i = 0; i < *actual_count; i++) {
        ret = i2c_slave_get_disc_info(comm, i, &discs[i]);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to get info for disc %ld", i);
            // Continue anyway to get as many as possible
        }
    }
    
    return ESP_OK;
}

esp_err_t i2c_slave_select_disc(i2c_slave_comm_t *comm, uint32_t disc_index) {
    if (!comm) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t data[4] = {
        (disc_index >> 24) & 0xFF,
        (disc_index >> 16) & 0xFF,
        (disc_index >> 8) & 0xFF,
        disc_index & 0xFF
    };
    
    return i2c_slave_send_command(comm, I2C_CMD_SELECT_DISC, data, sizeof(data),
                                 NULL, NULL);
}

esp_err_t i2c_slave_eject_disc(i2c_slave_comm_t *comm) {
    if (!comm) {
        return ESP_ERR_INVALID_ARG;
    }
    
    return i2c_slave_send_command(comm, I2C_CMD_EJECT_DISC, NULL, 0, NULL, NULL);
}

esp_err_t i2c_slave_get_disc_info(i2c_slave_comm_t *comm, uint32_t disc_index,
                                  disc_info_t *info) {
    if (!comm || !info) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t index_data[4] = {
        (disc_index >> 24) & 0xFF,
        (disc_index >> 16) & 0xFF,
        (disc_index >> 8) & 0xFF,
        disc_index & 0xFF
    };
    
    ESP_LOGI(TAG, "i2c_slave_get_disc_info: disc_index=%lu, data=[0x%02x,0x%02x,0x%02x,0x%02x]", 
             disc_index, index_data[0], index_data[1], index_data[2], index_data[3]);
    
    uint8_t response[sizeof(disc_info_t)];
    size_t response_len = sizeof(response);
    
    esp_err_t ret = i2c_slave_send_command(comm, I2C_CMD_GET_DISC_INFO,
                                           index_data, sizeof(index_data),
                                           response, &response_len);
    if (ret == ESP_OK) {
        memcpy(info, response, sizeof(disc_info_t));
        ESP_LOGI(TAG, "i2c_slave_get_disc_info: got response for disc_index=%lu, name=%s", 
                 disc_index, info->name);
    } else {
        ESP_LOGW(TAG, "i2c_slave_get_disc_info: failed for disc_index=%lu: %s", 
                 disc_index, esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t i2c_scan_bus(i2c_slave_comm_t *comm) {
    if (!comm || !comm->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Starting I2C bus scan...");
    int devices_found = 0;
    
    for (int addr = 0x08; addr < 0x78; addr++) {
        i2c_cmd_handle_t cmd_handle = i2c_cmd_link_create();
        if (cmd_handle == NULL) {
            continue;
        }
        
        i2c_master_start(cmd_handle);
        i2c_master_write_byte(cmd_handle, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd_handle);
        
        esp_err_t ret = i2c_master_cmd_begin(comm->port, cmd_handle, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd_handle);
        
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at address 0x%02X", addr);
            devices_found++;
        }
    }
    
    ESP_LOGI(TAG, "I2C bus scan complete. Found %d devices", devices_found);
    return ESP_OK;
}
