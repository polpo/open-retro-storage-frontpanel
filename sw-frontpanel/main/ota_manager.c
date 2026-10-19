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

#include "ota_manager.h"
#include "fw_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdalign.h>

static const char* TAG = "ota_manager";
#define OTA_MAX_RETRIES 3
#define OTA_CHUNK_TIMEOUT_MS 5000

// Static buffer for firmware chunks
alignas(4) static uint8_t chunk_buffer[PANEL_FIRMWARE_CHUNK_SIZE];

// Helper function to format SHA256 hash for logging
static void format_sha256_hex(const uint8_t* hash, char* hex_str, size_t hex_str_size) {
    for (int i = 0; i < 32; i++) {
        snprintf(hex_str + (i * 2), hex_str_size - (i * 2), "%02x", hash[i]);
    }
}

// Firmware validation function
static esp_err_t validate_firmware(const esp_partition_t* partition, const panel_firmware_info_t* fw_info) {
    if (!partition || !fw_info) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Validating firmware in partition %s", partition->label);

    // Read the first 1KB of the firmware to check the header
    uint8_t header[1024];
    esp_err_t ret = esp_partition_read(partition, 0, header, sizeof(header));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read firmware header: %s", esp_err_to_name(ret));
        return ret;
    }

    // Check ESP32 firmware magic bytes (ESP-IDF app header)
    // ESP32 firmware starts with 0xE9 (ESP_IMAGE_HEADER_MAGIC)
    if (header[0] != 0xE9) {
        ESP_LOGE(TAG, "Invalid firmware magic byte: 0x%02x (expected 0xE9)", header[0]);
        return ESP_ERR_INVALID_VERSION;
    }

    // Check segment count (should be reasonable)
    uint8_t segment_count = header[1];
    if (segment_count == 0 || segment_count > 16) {
        ESP_LOGE(TAG, "Invalid segment count: %d", segment_count);
        return ESP_ERR_INVALID_VERSION;
    }

    // Check flash mode is valid
    uint8_t flash_mode = header[2];

    if (flash_mode > 3) {  // Valid modes: 0=QIO, 1=QOUT, 2=DIO, 3=DOUT
        ESP_LOGE(TAG, "Invalid flash mode: %d", flash_mode);
        return ESP_ERR_INVALID_VERSION;
    }

    // Check entry point is reasonable (should be in IRAM)
    uint32_t entry_point = *(uint32_t*)&header[4];
    /*
    if (entry_point < 0x40080000 || entry_point > 0x400C0000) {
        ESP_LOGE(TAG, "Invalid entry point: 0x%08lx", entry_point);
        return ESP_ERR_INVALID_VERSION;
    }
    */

    ESP_LOGI(TAG, "Firmware validation passed:");
    ESP_LOGI(TAG, "  Magic: 0x%02x", header[0]);
    ESP_LOGI(TAG, "  Segments: %d", segment_count);
    ESP_LOGI(TAG, "  Flash mode: %d", flash_mode);
    ESP_LOGI(TAG, "  Entry point: 0x%08lx", entry_point);

    return ESP_OK;
}


// Helper function to parse version string into 32-bit integer
static uint32_t parse_version_string(const char* version_str) {
    if (!version_str) {
        return 0x00000000; // Default v0.0.0
    }
    // Encoding is 0xMMmmppPP: the low byte PP is 0xFF for a final release, or
    // 0-254 for a "-preN" prerelease (which sorts below the matching final).

    // pre stays 0xFF (final release) unless a -preN suffix overwrites it.
    // Use %u, not %hhu: ESP-IDF's nano newlib sscanf doesn't support the C99
    // "hh" length modifier, so %hhu silently fails to match.
    unsigned int major = 0, minor = 0, patch = 0, pre = 0xFF;

    // sscanf stops at the first field it can't match and returns the count it
    // filled: "0.3.99" yields 3 (a final release), "0.4.0-pre2" yields 4.
    int parsed = sscanf(version_str, "%u.%u.%u-pre%u", &major, &minor, &patch, &pre);
    if (parsed < 3) {
        ESP_LOGW(TAG, "Failed to parse version string '%s', using default", version_str);
        return 0x00000000;
    }

    return ((uint32_t)(major & 0xFF) << 24) | ((uint32_t)(minor & 0xFF) << 16) |
           ((uint32_t)(patch & 0xFF) << 8) | (uint32_t)(pre & 0xFF);
}


esp_err_t ota_manager_init(ota_manager_t* ota, host_comm_t* comm) {
    if (!ota || !comm) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(ota, 0, sizeof(ota_manager_t));
    ota->state = OTA_STATE_IDLE;
    ota->host_comm = comm;

    // Allocate SHA256 context
    ota->sha256_ctx = malloc(sizeof(mbedtls_sha256_context));
    if (!ota->sha256_ctx) {
        ESP_LOGE(TAG, "Failed to allocate SHA256 context");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "OTA manager initialized");
    return ESP_OK;
}

esp_err_t ota_manager_deinit(ota_manager_t* ota) {
    if (!ota) {
        return ESP_ERR_INVALID_ARG;
    }

    // Abort any ongoing update
    if (ota->state == OTA_STATE_DOWNLOADING) {
        ota_manager_abort(ota);
    }

    // Free SHA256 context
    if (ota->sha256_ctx) {
        if (ota->sha256_initialized) {
            mbedtls_sha256_free(ota->sha256_ctx);
        }
        free(ota->sha256_ctx);
        ota->sha256_ctx = NULL;
    }

    ota->state = OTA_STATE_IDLE;
    ESP_LOGI(TAG, "OTA manager deinitialized");
    return ESP_OK;
}

esp_err_t ota_manager_check_update(ota_manager_t* ota, bool* update_available) {
    if (!ota || !update_available) {
        return ESP_ERR_INVALID_ARG;
    }

    *update_available = false;
    ota->state = OTA_STATE_CHECKING;

    ESP_LOGI(TAG, "Checking for firmware update...");

    // Check for firmware and get info in one atomic operation
    esp_err_t ret = host_comm_check_firmware(ota->host_comm, &ota->firmware_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to check firmware: %s", esp_err_to_name(ret));
        ota->state = OTA_STATE_ERROR;
        ota->last_error = ret;
        return ret;
    }

    // Check if update is available
    if (ota->firmware_info.available) {
        uint32_t current_version = ota_manager_get_current_version();
        if (ota->firmware_info.version > current_version) {
            *update_available = true;
            char cur_str[20], avail_str[20];
            fw_version_format_panel(cur_str, sizeof(cur_str), current_version);
            fw_version_format_panel(avail_str, sizeof(avail_str), ota->firmware_info.version);
            ESP_LOGI(TAG, "Update available: v%s -> v%s", cur_str, avail_str);
        } else {
            ESP_LOGI(TAG, "Firmware is up to date");
        }
    } else {
        ESP_LOGI(TAG, "No firmware file found on host");
    }

    ota->state = OTA_STATE_IDLE;
    return ESP_OK;
}

esp_err_t ota_manager_start_update(ota_manager_t* ota) {
    if (!ota) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ota->state != OTA_STATE_IDLE) {
        ESP_LOGE(TAG, "Cannot start update - not in idle state");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting OTA update...");

    // Get next update partition
    ota->update_partition = esp_ota_get_next_update_partition(NULL);
    if (!ota->update_partition) {
        ESP_LOGE(TAG, "Failed to get update partition");
        ota->state = OTA_STATE_ERROR;
        ota->last_error = ESP_ERR_NOT_FOUND;
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Writing to partition: %s at offset 0x%lx",
             ota->update_partition->label, ota->update_partition->address);

    // Begin OTA update
    esp_err_t ret = esp_ota_begin(ota->update_partition, ota->firmware_info.size, &ota->update_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to begin OTA: %s", esp_err_to_name(ret));
        ota->state = OTA_STATE_ERROR;
        ota->last_error = ret;
        return ret;
    }

    // Initialize progress tracking
    ota->bytes_received = 0;
    ota->total_chunks = (ota->firmware_info.size + PANEL_FIRMWARE_CHUNK_SIZE - 1) / PANEL_FIRMWARE_CHUNK_SIZE;
    ota->current_chunk = 0;
    ota->retry_count = 0;

    // Initialize SHA256 calculation
    mbedtls_sha256_init(ota->sha256_ctx);
    int ret_sha = mbedtls_sha256_starts(ota->sha256_ctx, 0); // 0 = SHA256 (not SHA224)
    if (ret_sha != 0) {
        ESP_LOGE(TAG, "Failed to start SHA256 calculation: %d", ret_sha);
        esp_ota_abort(ota->update_handle);
        ota->state = OTA_STATE_ERROR;
        ota->last_error = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }
    ota->sha256_initialized = true;

    ota->state = OTA_STATE_DOWNLOADING;
    ESP_LOGI(TAG, "OTA update started - %lu chunks to download", ota->total_chunks);

    return ESP_OK;
}

esp_err_t ota_manager_process(ota_manager_t* ota) {
    if (!ota) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ota->state != OTA_STATE_DOWNLOADING) {
        return ESP_OK; // Nothing to do
    }

    // Check if all chunks received
    if (ota->current_chunk >= ota->total_chunks) {
        // Move to verification state
        ota->state = OTA_STATE_VERIFYING;

        ESP_LOGI(TAG, "All chunks received, verifying firmware...");

        // Finalize SHA256 calculation
        int ret_sha = mbedtls_sha256_finish(ota->sha256_ctx, ota->calculated_sha256);
        if (ret_sha != 0) {
            ESP_LOGE(TAG, "Failed to finalize SHA256 calculation: %d", ret_sha);
            ota->state = OTA_STATE_ERROR;
            ota->last_error = ESP_ERR_INVALID_STATE;
            esp_ota_abort(ota->update_handle);
            return ESP_ERR_INVALID_STATE;
        }

        // Verify SHA256
        if (memcmp(ota->calculated_sha256, ota->firmware_info.sha256, 32) != 0) {
            char expected_hex[65], calculated_hex[65];
            format_sha256_hex(ota->firmware_info.sha256, expected_hex, sizeof(expected_hex));
            format_sha256_hex(ota->calculated_sha256, calculated_hex, sizeof(calculated_hex));
            ESP_LOGE(TAG, "SHA256 verification failed!");
            ESP_LOGE(TAG, "Expected:   %s", expected_hex);
            ESP_LOGE(TAG, "Calculated: %s", calculated_hex);
            ota->state = OTA_STATE_ERROR;
            ota->last_error = ESP_ERR_INVALID_CRC;
            esp_ota_abort(ota->update_handle);
            return ESP_ERR_INVALID_CRC;
        }

        char sha256_hex[65];
        format_sha256_hex(ota->calculated_sha256, sha256_hex, sizeof(sha256_hex));
        ESP_LOGI(TAG, "SHA256 verification passed: %s", sha256_hex);

        // Validate firmware structure and header
        esp_err_t ret = validate_firmware(ota->update_partition, &ota->firmware_info);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Firmware validation failed: %s", esp_err_to_name(ret));
            ota->state = OTA_STATE_ERROR;
            ota->last_error = ret;
            esp_ota_abort(ota->update_handle);
            return ret;
        }

        // End OTA update
        ret = esp_ota_end(ota->update_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(ret));
            ota->state = OTA_STATE_ERROR;
            ota->last_error = ret;
            return ret;
        }

        // Set boot partition
        ret = esp_ota_set_boot_partition(ota->update_partition);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(ret));
            ota->state = OTA_STATE_ERROR;
            ota->last_error = ret;
            return ret;
        }

        // Remember this image came via the main board so the next boot can
        // insist on working host comms before committing to it.
        ota_manager_set_update_source(OTA_SOURCE_HOST);

        // Version will be automatically updated in esp_app_desc_t after restart

        ota->state = OTA_STATE_SUCCESS;
        ESP_LOGI(TAG, "OTA update successful! Restart to apply.");
        return ESP_OK;
    }

    // Request next chunk
    uint32_t offset = ota->current_chunk * PANEL_FIRMWARE_CHUNK_SIZE;
    size_t chunk_size = PANEL_FIRMWARE_CHUNK_SIZE;

    // Adjust size for last chunk
    if (offset + chunk_size > ota->firmware_info.size) {
        chunk_size = ota->firmware_info.size - offset;
    }

    ESP_LOGD(TAG, "Requesting chunk %lu/%lu (offset: 0x%lx, size: %d)",
             ota->current_chunk + 1, ota->total_chunks, offset, chunk_size);

    // Request firmware chunk from host
    esp_err_t ret = host_comm_read_firmware_chunk(ota->host_comm, offset, chunk_buffer, chunk_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read chunk %lu: %s", ota->current_chunk, esp_err_to_name(ret));

        ota->retry_count++;
        if (ota->retry_count >= OTA_MAX_RETRIES) {
            ESP_LOGE(TAG, "Max retries exceeded");
            ota->state = OTA_STATE_ERROR;
            ota->last_error = ret;
            esp_ota_abort(ota->update_handle);
            return ret;
        }

        // Retry same chunk
        vTaskDelay(pdMS_TO_TICKS(500));
        return ESP_OK;
    }

    // Update SHA256 calculation
    if (ota->sha256_initialized) {
        int ret_sha = mbedtls_sha256_update(ota->sha256_ctx, chunk_buffer, chunk_size);
        if (ret_sha != 0) {
            ESP_LOGE(TAG, "Failed to update SHA256 calculation: %d", ret_sha);
            ota->state = OTA_STATE_ERROR;
            ota->last_error = ESP_ERR_INVALID_STATE;
            esp_ota_abort(ota->update_handle);
            return ESP_ERR_INVALID_STATE;
        }
    }

    // Write chunk to flash
    ret = esp_ota_write(ota->update_handle, chunk_buffer, chunk_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write chunk %lu: %s", ota->current_chunk, esp_err_to_name(ret));
        ota->state = OTA_STATE_ERROR;
        ota->last_error = ret;
        esp_ota_abort(ota->update_handle);
        return ret;
    }

    // Update progress
    ota->bytes_received += chunk_size;
    ota->current_chunk++;
    ota->retry_count = 0; // Reset retry count on success

    // Log progress every 10%
    uint8_t progress = ota_manager_get_progress(ota);
    if (progress % 10 == 0) {
        ESP_LOGI(TAG, "OTA progress: %d%%", progress);
    }

    return ESP_OK;
}

ota_state_t ota_manager_get_state(ota_manager_t* ota) {
    if (!ota) {
        return OTA_STATE_IDLE;
    }
    return ota->state;
}

uint8_t ota_manager_get_progress(ota_manager_t* ota) {
    if (!ota || ota->firmware_info.size == 0) {
        return 0;
    }

    if (ota->state == OTA_STATE_SUCCESS) {
        return 100;
    }

    return (uint8_t)((ota->bytes_received * 100) / ota->firmware_info.size);
}

esp_err_t ota_manager_abort(ota_manager_t* ota) {
    if (!ota) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ota->state == OTA_STATE_DOWNLOADING) {
        esp_ota_abort(ota->update_handle);
        ESP_LOGW(TAG, "OTA update aborted");
    }

    ota->state = OTA_STATE_IDLE;
    return ESP_OK;
}

#define OTA_NVS_NAMESPACE "ota"
#define OTA_NVS_KEY_SOURCE "src"

esp_err_t ota_manager_set_update_source(ota_update_source_t source) {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS to record update source: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = nvs_set_u8(handle, OTA_NVS_KEY_SOURCE, (uint8_t)source);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to record update source: %s", esp_err_to_name(ret));
    }
    return ret;
}

ota_update_source_t ota_manager_take_update_source(void) {
    nvs_handle_t handle;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return OTA_SOURCE_NONE;
    }
    uint8_t value = OTA_SOURCE_NONE;
    if (nvs_get_u8(handle, OTA_NVS_KEY_SOURCE, &value) == ESP_OK) {
        nvs_erase_key(handle, OTA_NVS_KEY_SOURCE);
        nvs_commit(handle);
    }
    nvs_close(handle);
    return (ota_update_source_t)value;
}

esp_err_t ota_manager_mark_valid(void) {
    esp_err_t ret = esp_ota_mark_app_valid_cancel_rollback();

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "App marked as valid, rollback cancelled");
    } else {
        ESP_LOGE(TAG, "Failed to mark app as valid: %s", esp_err_to_name(ret));
    }

    return ret;
}

uint32_t ota_manager_get_current_version(void) {
    // Get current app description from ESP-IDF
    const esp_app_desc_t* app_desc = esp_app_get_description();
    if (!app_desc) {
        ESP_LOGE(TAG, "Failed to get app description");
        return 0x00000000; // Default v0.0.0
    }

    // Parse version from app description
    uint32_t version = parse_version_string(app_desc->version);

    ESP_LOGI(TAG, "Current firmware version: %s -> 0x%08lX", app_desc->version, version);

    return version;
}

