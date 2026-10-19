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

#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include "esp_err.h"
#include "esp_ota_ops.h"
#include "host_comm.h"
#include "panel_protocol_defs.h"
#include "mbedtls/sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

// OTA state machine
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_CHECKING,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_APPLYING,
    OTA_STATE_SUCCESS,
    OTA_STATE_ERROR
} ota_state_t;

// OTA manager structure
typedef struct {
    ota_state_t state;
    host_comm_t* host_comm;

    // OTA operation handles
    esp_ota_handle_t update_handle;
    const esp_partition_t* update_partition;

    // Firmware info
    panel_firmware_info_t firmware_info;

    // Progress tracking
    uint32_t bytes_received;
    uint32_t total_chunks;
    uint32_t current_chunk;

    // SHA256 verification
    uint8_t calculated_sha256[32];
    mbedtls_sha256_context* sha256_ctx;
    bool sha256_initialized;

    // Error tracking
    esp_err_t last_error;
    int retry_count;
} ota_manager_t;

// Public API
esp_err_t ota_manager_init(ota_manager_t* ota, host_comm_t* comm);
esp_err_t ota_manager_deinit(ota_manager_t* ota);

// Check if firmware update is available
esp_err_t ota_manager_check_update(ota_manager_t* ota, bool* update_available);

// Start OTA update process
esp_err_t ota_manager_start_update(ota_manager_t* ota);

// Process OTA update (call periodically)
esp_err_t ota_manager_process(ota_manager_t* ota);

// Get current state
ota_state_t ota_manager_get_state(ota_manager_t* ota);

// Get progress percentage (0-100)
uint8_t ota_manager_get_progress(ota_manager_t* ota);

// Abort update
esp_err_t ota_manager_abort(ota_manager_t* ota);

// Mark current firmware as valid (call after successful boot)
esp_err_t ota_manager_mark_valid(void);

// Where the pending OTA image came from. Persisted in NVS across the reboot
// into the new image so boot-time validation can decide what to require
// before cancelling rollback.
typedef enum {
    OTA_SOURCE_NONE = 0,
    OTA_SOURCE_HOST = 1,    // Fetched from the main board (host_comm)
    OTA_SOURCE_DIRECT = 2   // Uploaded directly (e.g. web upload)
} ota_update_source_t;

// Record the source of the image that was just written and set as boot partition
esp_err_t ota_manager_set_update_source(ota_update_source_t source);

// Read and clear the recorded source (returns OTA_SOURCE_NONE if not set)
ota_update_source_t ota_manager_take_update_source(void);

// Get current firmware version
uint32_t ota_manager_get_current_version(void);


#ifdef __cplusplus
}
#endif

#endif // OTA_MANAGER_H
