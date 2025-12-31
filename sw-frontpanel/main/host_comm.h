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

#ifndef HOST_COMM_H
#define HOST_COMM_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "transport.h"
#include "panel_protocol_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOST_COMM_MAX_DATA_SIZE 256
#define HOST_COMM_TIMEOUT_MS 1000

// Use playback status structure from protocol definitions
typedef panel_playback_status_t playback_status_t;

// Host communication handle
typedef struct {
    transport_handle_t transport;
    bool initialized;
} host_comm_t;

// Public API
esp_err_t host_comm_init(host_comm_t *comm, const transport_config_t *config);
esp_err_t host_comm_deinit(host_comm_t *comm);

// Directory browsing functions
esp_err_t host_comm_get_entry_count(host_comm_t *comm, uint32_t *count);
esp_err_t host_comm_get_entry_info(host_comm_t *comm, uint32_t index, dir_entry_info_t *info);
esp_err_t host_comm_select_entry(host_comm_t *comm, int32_t index);  // -1 = parent dir, >=0 = select entry
esp_err_t host_comm_get_current_path(host_comm_t *comm, char *path, size_t max_len);

// Image management functions
esp_err_t host_comm_select_prev_image(host_comm_t *comm);
esp_err_t host_comm_select_next_image(host_comm_t *comm);
esp_err_t host_comm_eject_image(host_comm_t *comm);
esp_err_t host_comm_get_loaded_image_status(host_comm_t *comm, loaded_image_status_t *status);

// Status functions
esp_err_t host_comm_get_device_status(host_comm_t *comm, uint8_t *status);
esp_err_t host_comm_get_playback_status(host_comm_t *comm, playback_status_t *status);

// Firmware update functions
esp_err_t host_comm_check_firmware(host_comm_t *comm);
esp_err_t host_comm_get_firmware_info(host_comm_t *comm, panel_firmware_info_t *info);
esp_err_t host_comm_read_firmware_chunk(host_comm_t *comm, uint32_t offset,
                                        uint8_t *buffer, size_t size);

// File upload functions
esp_err_t host_comm_start_file_upload(host_comm_t *comm, const char *filename, uint32_t file_size);
esp_err_t host_comm_write_file_chunk(host_comm_t *comm, const uint8_t *data, size_t size);
esp_err_t host_comm_finish_file_upload(host_comm_t *comm, uint8_t *result_code, uint8_t *file_hash);

// RP2350 (main board) firmware functions
esp_err_t host_comm_get_rp2350_fw_status(host_comm_t *comm, rp2350_fw_status_t *status);
esp_err_t host_comm_start_rp2350_update(host_comm_t *comm);

// Command status polling (immediate read, doesn't interfere with running commands)
esp_err_t host_comm_get_command_status(host_comm_t *comm, panel_command_status_t *status);

const char* host_comm_get_transport_name(host_comm_t *comm);

#ifdef __cplusplus
}
#endif

#endif // HOST_COMM_H
