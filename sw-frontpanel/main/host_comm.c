#include "host_comm.h"
#include "panel_protocol_defs.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdalign.h>

static const char *TAG = "host_comm";

// Static buffers for zero-copy operations
alignas(4) static uint8_t tx_buffer[PANEL_PROTOCOL_MAX_PAYLOAD];
alignas(4) static uint8_t rx_buffer[PANEL_PROTOCOL_MAX_PAYLOAD];

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

// Helper function to poll for async operation completion and retrieve result
static esp_err_t host_comm_poll_async_result(host_comm_t *comm, uint32_t timeout_ms, uint32_t poll_interval_ms,
                                             size_t expected_size, uint8_t **result_data, size_t *actual_size) {
    if (!comm) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t elapsed_ms = 0;
    bool ready = false;
    uint16_t response_size = 0;

    while (elapsed_ms < timeout_ms && !ready) {
        vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
        elapsed_ms += poll_interval_ms;

        esp_err_t ret = transport_poll_async_status(&comm->transport, &ready, &response_size,
                                                    rx_buffer, expected_size);
        if (ret != ESP_OK) {
            return ret;
        }

        /* ESP_LOGI(TAG, "Poll response: ready=%u, size=%u", ready, response_size); */
    }

    if (!ready) {
        ESP_LOGE(TAG, "Async operation timeout after %lu ms", elapsed_ms);
        return ESP_ERR_TIMEOUT;
    }

    if (expected_size > 0 && response_size != expected_size) {
        ESP_LOGE(TAG, "Unexpected response size: %u (expected %zu)", response_size, expected_size);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (result_data) {
        *result_data = rx_buffer;
    }
    if (actual_size) {
        *actual_size = response_size;
    }

    return ESP_OK;
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

    // Poll for completion with 1 second timeout
    uint8_t *result_data;
    size_t result_size;
    ret = host_comm_poll_async_result(comm, 1000, 10, 4, &result_data, &result_size);
    if (ret != ESP_OK) {
        return ret;
    }

    // Parse the 4-byte big-endian disc count
    *count = (result_data[0] << 24) | (result_data[1] << 16) | (result_data[2] << 8) | result_data[3];
    ESP_LOGI(TAG, "Disc count: %lu", *count);

    return ESP_OK;
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

    if (disc_index <= 0xffff) {
        // Use argument byte for small indices
        return transport_two_phase_transaction(&comm->transport,
                                               PANEL_CMD_SELECT_DISC, (uint16_t)disc_index,
                                               NULL, 0,  // No write data
                                               NULL, 0);  // No read data
    } else {
        // Use extended format for large indices
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
    if (disc_index <= 0xffff) {
        // Use argument byte for small indices
        ret = transport_two_phase_transaction(&comm->transport,
                                              PANEL_CMD_START_DISC_INFO, (uint16_t)disc_index,
                                              NULL, 0,  // No write data
                                              NULL, 0);  // No read data
    } else {
        // Use extended format for large indices
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

    // Poll for completion with 1 second timeout
    uint8_t *result_data;
    size_t result_size;
    ret = host_comm_poll_async_result(comm, 1000, 10, sizeof(disc_info_t), &result_data, &result_size);
    if (ret != ESP_OK) {
        return ret;
    }

    // Copy result to caller's buffer
    memcpy(info, result_data, sizeof(disc_info_t));
    ESP_LOGI(TAG, "Got disc info: %s", info->name);

    return ret;
}

esp_err_t host_comm_check_firmware(host_comm_t *comm) {
    if (!comm || !comm->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Checking for firmware updates...");

    // Send CHECK_FIRMWARE command (async operation)
    return transport_two_phase_transaction(&comm->transport,
                                           PANEL_CMD_CHECK_FIRMWARE, PANEL_ARG_IGNORED,
                                           NULL, 0,  // No write data
                                           NULL, 0); // No immediate read data
}

esp_err_t host_comm_get_firmware_info(host_comm_t *comm, panel_firmware_info_t *info) {
    if (!comm || !comm->initialized || !info) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Getting firmware info...");

    // Poll for async result first
    uint8_t *result_data;
    size_t result_size;
    esp_err_t ret = host_comm_poll_async_result(comm, 2000, 10, sizeof(panel_firmware_info_t), &result_data, &result_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get firmware check result: %s", esp_err_to_name(ret));
        return ret;
    }

    // Copy result to caller's buffer
    memcpy(info, result_data, sizeof(panel_firmware_info_t));

    char sha256_hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(sha256_hex + (i * 2), sizeof(sha256_hex) - (i * 2), "%02x", info->sha256[i]);
    }
    ESP_LOGI(TAG, "Firmware info - Available: %d, Size: %lu, Version: 0x%08lx, SHA256: %s",
             info->available, info->size, info->version, sha256_hex);

    return ESP_OK;
}

esp_err_t host_comm_read_firmware_chunk(host_comm_t *comm, uint32_t offset,
                                        uint8_t *buffer, size_t size) {
    if (!comm || !comm->initialized || !buffer || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (size > PANEL_FIRMWARE_CHUNK_SIZE) {
        ESP_LOGE(TAG, "Chunk size too large: %d > %d", size, PANEL_FIRMWARE_CHUNK_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGD(TAG, "Reading firmware chunk at offset 0x%lx, size %d", offset, size);

    // Prepare offset in tx_buffer (little endian)
    tx_buffer[0] = offset & 0xFF;
    tx_buffer[1] = (offset >> 8) & 0xFF;
    tx_buffer[2] = (offset >> 16) & 0xFF;
    tx_buffer[3] = (offset >> 24) & 0xFF;

    // Send START_FIRMWARE_READ command with offset
    esp_err_t ret = transport_two_phase_transaction(&comm->transport,
                                                    PANEL_CMD_START_FIRMWARE_READ, PANEL_ARG_EXTENDED,
                                                    tx_buffer, 4,  // Send offset
                                                    NULL, 0);      // No immediate read data
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send firmware read command: %s", esp_err_to_name(ret));
        return ret;
    }

    // Wait for chunk to be ready and read it
    uint8_t *result_data;
    size_t result_size;
    ret = host_comm_poll_async_result(comm, 5000, 10, size, &result_data, &result_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get firmware chunk: %s", esp_err_to_name(ret));
        return ret;
    }

    if (result_size != size) {
        ESP_LOGE(TAG, "Firmware chunk size mismatch: expected %d, got %d", size, result_size);
        return ESP_ERR_INVALID_SIZE;
    }

    // Copy chunk data to caller's buffer
    memcpy(buffer, result_data, size);

    ESP_LOGD(TAG, "Successfully read firmware chunk: %d bytes", size);
    return ESP_OK;
}

esp_err_t host_comm_start_file_upload(host_comm_t *comm, const char *filename, uint32_t file_size, const uint8_t *expected_hash) {
    if (!comm || !comm->initialized || !filename) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting file upload: %s (%lu bytes)", filename, file_size);

    // Prepare payload with file upload start structure
    size_t filename_len = strlen(filename);
    if (filename_len > (PANEL_PROTOCOL_MAX_PAYLOAD - sizeof(panel_file_upload_start_t) - 1)) {
        ESP_LOGE(TAG, "Filename too long: %d bytes", filename_len);
        return ESP_ERR_INVALID_ARG;
    }

    panel_file_upload_start_t *upload_start = (panel_file_upload_start_t *)tx_buffer;
    upload_start->file_size = file_size;
    upload_start->filename_len = filename_len;

    // Copy SHA256 hash if provided
    if (expected_hash) {
        memcpy(upload_start->hash, expected_hash, 32);
    } else {
        memset(upload_start->hash, 0, 32); // Clear hash if not provided
    }

    // Copy filename after the structure (null-terminated)
    memcpy(tx_buffer + sizeof(panel_file_upload_start_t), filename, filename_len);
    tx_buffer[sizeof(panel_file_upload_start_t) + filename_len] = '\0';

    size_t total_payload_size = sizeof(panel_file_upload_start_t) + filename_len + 1;

    // Send START_FILE_UPLOAD command (async operation)
    esp_err_t ret = transport_two_phase_transaction(&comm->transport,
                                                   PANEL_CMD_START_FILE_UPLOAD, PANEL_ARG_EXTENDED,
                                                   tx_buffer, total_payload_size,
                                                   NULL, 0); // No immediate read data

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start file upload: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "File upload start command sent successfully");
    return ESP_OK;
}

esp_err_t host_comm_write_file_chunk(host_comm_t *comm, const uint8_t *data, size_t size) {
    if (!comm || !comm->initialized || !data || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (size > PANEL_FILE_CHUNK_SIZE) {
        ESP_LOGE(TAG, "Chunk size too large: %d bytes (max %d)", size, PANEL_FILE_CHUNK_SIZE);
        return ESP_ERR_INVALID_ARG;
    }

    /* ESP_LOGD(TAG, "Writing file chunk: %d bytes", size); */

    // Copy chunk data to tx buffer
    memcpy(tx_buffer, data, size);
   
    // Calc CRC16 of chunk 
    uint16_t chunk_crc16 = ~esp_rom_crc16_be((uint16_t)~0xffff, data, size);

    // Send WRITE_FILE_CHUNK command (async operation)
    esp_err_t ret = transport_two_phase_transaction(&comm->transport,
                                                   PANEL_CMD_WRITE_FILE_CHUNK, chunk_crc16,
                                                   tx_buffer, size,
                                                   NULL, 0); // No immediate read data

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write file chunk: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = host_comm_poll_async_result(comm, 1000, 1, 0, NULL, NULL);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed async result after write: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ESP_LOGD(TAG, "File chunk written successfully"); */
    return ESP_OK;
}

esp_err_t host_comm_finish_file_upload(host_comm_t *comm, uint8_t *result_code) {
    if (!comm || !comm->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Finishing file upload");

    // Send FINISH_FILE_UPLOAD command (async operation)
    esp_err_t ret = transport_two_phase_transaction(&comm->transport,
                                                   PANEL_CMD_FINISH_FILE_UPLOAD, PANEL_ARG_IGNORED,
                                                   NULL, 0,  // No write data
                                                   NULL, 0); // No immediate read data

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send finish file upload command: %s", esp_err_to_name(ret));
        return ret;
    }

    // Poll for async result (1 byte result code)
    uint8_t *result_data;
    size_t result_size;
    ret = host_comm_poll_async_result(comm, 5000, 10, 1, &result_data, &result_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get file upload result: %s", esp_err_to_name(ret));
        return ret;
    }

    if (result_size != 1) {
        ESP_LOGE(TAG, "Upload result size mismatch: expected 1, got %d", result_size);
        return ESP_ERR_INVALID_SIZE;
    }

    if (result_code) {
        *result_code = result_data[0];
    }

    if (result_data[0] == PANEL_UPLOAD_OK) {
        ESP_LOGI(TAG, "File upload completed successfully");
    } else {
        ESP_LOGE(TAG, "File upload failed with code: 0x%02X", result_data[0]);
    }

    return ESP_OK;
}

