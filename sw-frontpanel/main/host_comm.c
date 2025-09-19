#include "host_comm.h"
#include "panel_protocol_defs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "host_comm";

// Static buffers for zero-copy operations
static uint8_t tx_buffer[PANEL_PROTOCOL_MAX_PAYLOAD];
static uint8_t rx_buffer[PANEL_PROTOCOL_MAX_PAYLOAD + 8];  // Extra space for status bytes

esp_err_t host_comm_init(host_comm_t *comm, const transport_config_t *config) {
    if (!comm || !config) {
        return ESP_ERR_INVALID_ARG;
    }

    // Initialize transport layer
    esp_err_t ret = transport_init(&comm->transport, config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize transport: %s", esp_err_to_name(ret));
        return ret;
    }

    comm->initialized = true;
    
    ESP_LOGI(TAG, "Host communication initialized using %s transport (device addr: 0x%02X)",
             transport_get_name(&comm->transport), config->device_addr);
    
    return ESP_OK;
}

esp_err_t host_comm_deinit(host_comm_t *comm) {
    if (!comm || !comm->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = transport_deinit(&comm->transport);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deinitialize transport: %s", esp_err_to_name(ret));
        return ret;
    }

    comm->initialized = false;
    ESP_LOGI(TAG, "Host communication deinitialized");
    
    return ESP_OK;
}

const char* host_comm_get_transport_name(host_comm_t *comm) {
    if (!comm || !comm->initialized) {
        return "Not initialized";
    }
    return transport_get_name(&comm->transport);
}

// Helper function to send command and wait for response
static esp_err_t host_comm_write_command(host_comm_t *comm, host_command_t cmd,
                                         const uint8_t *data, size_t data_len) {
    if (!comm || !comm->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "Writing command: 0x%02x, data_len=%u", cmd, data_len);

    // Prepare command buffer
    uint8_t cmd_buffer[HOST_COMM_MAX_DATA_SIZE + 2];
    size_t cmd_len = 0;
    
    // Add command byte
    cmd_buffer[cmd_len++] = cmd;
    
    // Add data length (for variable length commands)
    if (cmd == HOST_CMD_SELECT_DISC || cmd == HOST_CMD_GET_DISC_INFO) {
        // These commands have fixed 4-byte data
        cmd_buffer[cmd_len++] = 4;
        if (data && data_len >= 4) {
            memcpy(&cmd_buffer[cmd_len], data, 4);
            cmd_len += 4;
        }
    } else if (data && data_len > 0) {
        cmd_buffer[cmd_len++] = (uint8_t)data_len;
        memcpy(&cmd_buffer[cmd_len], data, data_len);
        cmd_len += data_len;
    } else {
        cmd_buffer[cmd_len++] = 0;  // No data
    }

    // Send command via transport
    esp_err_t ret = transport_write(&comm->transport, cmd_buffer, cmd_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write command 0x%02X: %s", cmd, esp_err_to_name(ret));
    }

    return ret;
}

// Helper function to read response
static esp_err_t host_comm_read_response(host_comm_t *comm, uint8_t *response, size_t *response_len) {
    if (!comm || !comm->initialized || !response || !response_len) {
        return ESP_ERR_INVALID_ARG;
    }

    // Read response via transport
    esp_err_t ret = transport_read(&comm->transport, response, *response_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read response: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t host_comm_send_command(host_comm_t *comm, host_command_t cmd,
                                 const uint8_t *data, size_t data_len,
                                 uint8_t *response, size_t *response_len) {
    if (!comm || !comm->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Sending command: 0x%02x", cmd);

    // Send command
    esp_err_t ret = host_comm_write_command(comm, cmd, data, data_len);
    if (ret != ESP_OK) {
        return ret;
    }

    // Wait a bit for processing
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Poll for response status
    uint8_t status = HOST_STATUS_NO_CMD;
    size_t status_len = 1;
    int retries = 10;

    while (retries-- > 0) {
        ret = host_comm_read_response(comm, &status, &status_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read status: %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGI(TAG, "Status: 0x%02x", status);

        if (status == HOST_STATUS_READY) {
            // Response is ready, read it if requested
            if (response && response_len && *response_len > 0) {
                ret = host_comm_read_response(comm, response, response_len);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to read response data: %s", esp_err_to_name(ret));
                    return ret;
                }
            }
            return ESP_OK;
        } else if (status == HOST_STATUS_ERROR || status == HOST_STATUS_INVALID_CMD) {
            ESP_LOGE(TAG, "Command failed with status: 0x%02x", status);
            return ESP_ERR_INVALID_RESPONSE;
        } else if (status == HOST_STATUS_BUSY) {
            // Still processing, wait a bit
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else if (status == HOST_STATUS_NO_CMD) {
            // No command received, might need to resend
            ESP_LOGW(TAG, "No command status, retrying");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ESP_LOGE(TAG, "Command timeout");
    return ESP_ERR_TIMEOUT;
}

esp_err_t host_comm_get_status(host_comm_t *comm, host_status_t *status) {
    if (!comm || !status) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Getting disc status");

    uint8_t disc_status;
    esp_err_t ret = transport_two_phase_transaction(&comm->transport,
                                                    PANEL_CMD_GET_DISC_STATUS, PANEL_ARG_IGNORED,
                                                    NULL, 0,  // No write data
                                                    &disc_status, 1);  // Read 1 byte
    if (ret == ESP_OK) {
        *status = (host_status_t)disc_status;
    }

    return ret;
}

esp_err_t host_comm_get_disc_count(host_comm_t *comm, uint32_t *count) {
    if (!comm || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    // Start the disc count operation
    esp_err_t ret = transport_two_phase_transaction(&comm->transport,
                                                    PANEL_CMD_START_DISC_COUNT, PANEL_ARG_IGNORED,
                                                    NULL, 0,  // No write data
                                                    NULL, 0);  // No read data
    if (ret != ESP_OK) {
        return ret;
    }

    // Poll until ready and get result in optimized single operation
    bool ready = false;
    uint16_t response_size = 0;
    int retries = 10;

    while (retries-- > 0 && !ready) {
        vTaskDelay(pdMS_TO_TICKS(100));  // Wait 100ms between polls

        ret = transport_two_phase_transaction(&comm->transport,
                                              PANEL_CMD_POLL_OP_READY, PANEL_ARG_IGNORED,
                                              NULL, 0,  // No write data
                                              rx_buffer, sizeof(rx_buffer));  // Transport handles optimization
        if (ret != ESP_OK) {
            return ret;
        }

        ready = (rx_buffer[0] != 0);
        response_size = rx_buffer[1] | (rx_buffer[2] << 8);

        ESP_LOGI(TAG, "Poll response: ready=%u, size=%u", ready, response_size);
    }

    if (!ready) {
        ESP_LOGE(TAG, "Disc count operation timeout");
        return ESP_ERR_TIMEOUT;
    }

    if (response_size != 4) {
        ESP_LOGE(TAG, "Unexpected disc count response size: %u", response_size);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Result is already in rx_buffer after the 3-byte status response (zero-copy!)
    uint8_t *result = rx_buffer + 3;
    *count = (result[0] << 24) | (result[1] << 16) | (result[2] << 8) | result[3];
    ESP_LOGI(TAG, "Disc count: %lu", *count);

    return ret;
}

esp_err_t host_comm_get_disc_list(host_comm_t *comm, disc_info_t *discs,
                                  size_t max_discs, size_t *disc_count) {
    if (!comm || !discs || !disc_count) {
        return ESP_ERR_INVALID_ARG;
    }

    // First get the count
    uint32_t count;
    esp_err_t ret = host_comm_get_disc_count(comm, &count);
    if (ret != ESP_OK) {
        return ret;
    }

    *disc_count = (count < max_discs) ? count : max_discs;

    // Get info for each disc
    for (size_t i = 0; i < *disc_count; i++) {
        ret = host_comm_get_disc_info(comm, i, &discs[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get info for disc %u: %s", i, esp_err_to_name(ret));
            break;
        }
    }

    return ret;
}

esp_err_t host_comm_select_disc(host_comm_t *comm, uint32_t disc_index) {
    if (!comm) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Selecting disc: %lu", disc_index);

    if (disc_index <= 255) {
        // Use argument byte for small indices
        return transport_two_phase_transaction(&comm->transport,
                                               PANEL_CMD_SELECT_DISC, (uint8_t)disc_index,
                                               NULL, 0,  // No write data
                                               NULL, 0);  // No read data
    } else {
        // Use extended format for large indices - use static TX buffer (zero-copy!)
        tx_buffer[0] = (disc_index >> 24) & 0xFF;
        tx_buffer[1] = (disc_index >> 16) & 0xFF;
        tx_buffer[2] = (disc_index >> 8) & 0xFF;
        tx_buffer[3] = disc_index & 0xFF;

        return transport_two_phase_transaction(&comm->transport,
                                               PANEL_CMD_SELECT_DISC, PANEL_ARG_EXTENDED,
                                               tx_buffer, 4,  // Write from static buffer
                                               NULL, 0);  // No read data
    }
}

esp_err_t host_comm_eject_disc(host_comm_t *comm) {
    if (!comm) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Ejecting disc");

    return transport_two_phase_transaction(&comm->transport,
                                           PANEL_CMD_EJECT_DISC, PANEL_ARG_IGNORED,
                                           NULL, 0,  // No write data
                                           NULL, 0);  // No read data
}

esp_err_t host_comm_get_disc_info(host_comm_t *comm, uint32_t disc_index,
                                  disc_info_t *info) {
    if (!comm || !info) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Getting info for disc: %lu", disc_index);

    // Start the disc info operation
    esp_err_t ret;
    if (disc_index <= 255) {
        // Use argument byte for small indices
        ret = transport_two_phase_transaction(&comm->transport,
                                              PANEL_CMD_START_DISC_INFO, (uint8_t)disc_index,
                                              NULL, 0,  // No write data
                                              NULL, 0);  // No read data
    } else {
        // Use extended format for large indices - use static TX buffer (zero-copy!)
        tx_buffer[0] = (disc_index >> 24) & 0xFF;
        tx_buffer[1] = (disc_index >> 16) & 0xFF;
        tx_buffer[2] = (disc_index >> 8) & 0xFF;
        tx_buffer[3] = disc_index & 0xFF;

        ret = transport_two_phase_transaction(&comm->transport,
                                              PANEL_CMD_START_DISC_INFO, PANEL_ARG_EXTENDED,
                                              tx_buffer, 4,  // Write from static buffer
                                              NULL, 0);  // No read data
    }

    if (ret != ESP_OK) {
        return ret;
    }

    // Poll until ready and get result in optimized single operation
    bool ready = false;
    uint16_t response_size = 0;
    int retries = 10;

    while (retries-- > 0 && !ready) {
        vTaskDelay(pdMS_TO_TICKS(100));  // Wait 100ms between polls

        ret = transport_two_phase_transaction(&comm->transport,
                                              PANEL_CMD_POLL_OP_READY, PANEL_ARG_IGNORED,
                                              NULL, 0,  // No write data
                                              rx_buffer, sizeof(rx_buffer));  // Transport handles optimization
        if (ret != ESP_OK) {
            return ret;
        }

        ready = (rx_buffer[0] != 0);
        response_size = rx_buffer[1] | (rx_buffer[2] << 8);

        ESP_LOGI(TAG, "Poll response: ready=%u, size=%u", ready, response_size);
    }

    if (!ready) {
        ESP_LOGE(TAG, "Disc info operation timeout");
        return ESP_ERR_TIMEOUT;
    }

    if (response_size != sizeof(disc_info_t)) {
        ESP_LOGE(TAG, "Unexpected disc info response size: %u (expected %zu)", response_size, sizeof(disc_info_t));
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Result is already in rx_buffer after the 3-byte status response (zero-copy!)
    disc_info_t *result = (disc_info_t*)(rx_buffer + 3);
    memcpy(info, result, sizeof(disc_info_t));  // Copy to caller's buffer
    ESP_LOGI(TAG, "Got disc info: %s", info->name);

    return ret;
}

