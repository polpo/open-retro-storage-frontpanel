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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
    SemaphoreHandle_t mutex;  // Protects transport access; non-NULL means the handle is usable
    bool link_up;             // The main board is answering. Cleared while the handle stays up,
                              // so callers must not read it as "safe to call host_comm_*".
} host_comm_t;

// Public API
esp_err_t host_comm_init(host_comm_t *comm, const transport_config_t *config);
esp_err_t host_comm_deinit(host_comm_t *comm);

// Directory browsing functions
//
// For devices like PicoIDE that keep per-device browse directories/file
// filters, device_index sends the index of the device. BlueSCSI has one common
// browse directory and ignores device_index.
esp_err_t host_comm_get_entry_count(host_comm_t *comm, uint16_t device_index, uint32_t *count);
esp_err_t host_comm_get_entry_info(host_comm_t *comm, uint16_t device_index, uint32_t index, dir_entry_info_t *info);
esp_err_t host_comm_select_entry(host_comm_t *comm, int32_t index, uint16_t device_index);  // -1 = parent dir, >=0 = select entry
esp_err_t host_comm_get_current_path(host_comm_t *comm, uint16_t device_index, char *path, size_t max_len);

// Image management functions (device_index selects target device, 0 for default/single device)
esp_err_t host_comm_select_prev_image(host_comm_t *comm, uint16_t device_index);
esp_err_t host_comm_select_next_image(host_comm_t *comm, uint16_t device_index);
esp_err_t host_comm_eject_image(host_comm_t *comm, uint16_t device_index);
esp_err_t host_comm_get_loaded_image_status(host_comm_t *comm, uint16_t device_index, loaded_image_status_t *status);

// Status functions (device_index selects target device, 0 for default/single device)
esp_err_t host_comm_get_device_status(host_comm_t *comm, uint16_t device_index, uint8_t *status);
esp_err_t host_comm_get_playback_status(host_comm_t *comm, uint16_t device_index, playback_status_t *status);

// Validated liveness probe: fetches playback status and checks it carries
// PANEL_ALIVE_MAGIC. ESP_ERR_INVALID_RESPONSE if the transaction completed but
// no live main board answered (e.g. reading a floating SPI bus).
esp_err_t host_comm_probe_alive(host_comm_t *comm, uint16_t device_index);

// Device list
esp_err_t host_comm_get_device_list(host_comm_t *comm, device_list_response_t *response, size_t max_size);

// Firmware update functions
esp_err_t host_comm_check_firmware(host_comm_t *comm, panel_firmware_info_t *info);
esp_err_t host_comm_read_firmware_chunk(host_comm_t *comm, uint32_t offset,
                                        uint8_t *buffer, size_t size);

// File upload functions (filename is full path, e.g., "/picoide.ini")
esp_err_t host_comm_start_file_upload(host_comm_t *comm, const char *filename, uint32_t file_size);
esp_err_t host_comm_write_file_chunk(host_comm_t *comm, const uint8_t *data, size_t size);
esp_err_t host_comm_finish_file_upload(host_comm_t *comm, uint8_t *result_code, uint8_t *file_hash);

// File download functions (filename is full path, e.g., "/picoide.ini")
esp_err_t host_comm_start_file_download(host_comm_t *comm, const char *filename, uint32_t *file_size);
esp_err_t host_comm_read_file_chunk(host_comm_t *comm, uint32_t chunk_index, uint8_t *buffer, size_t *bytes_read);

// File delete/rename (paths are full paths, e.g., "/dir/image.iso"). On ESP_OK,
// *result_code holds the PANEL_DELETE_*/PANEL_RENAME_* code from the main board.
esp_err_t host_comm_delete_file(host_comm_t *comm, const char *path, uint8_t *result_code);
esp_err_t host_comm_rename_file(host_comm_t *comm, const char *old_path,
                                const char *new_path, uint8_t *result_code);

// Create an empty file / a directory (path is full path). On ESP_OK, *result_code
// holds the PANEL_TOUCH_*/PANEL_MKDIR_* code from the main board.
esp_err_t host_comm_touch_file(host_comm_t *comm, const char *path, uint8_t *result_code);
esp_err_t host_comm_mkdir(host_comm_t *comm, const char *path, uint8_t *result_code);

// RP2350 (main board) firmware functions
esp_err_t host_comm_get_rp2350_fw_status(host_comm_t *comm, rp2350_fw_status_t *status);
esp_err_t host_comm_start_rp2350_update(host_comm_t *comm);

// Command status polling (immediate read, doesn't interfere with running commands)
esp_err_t host_comm_get_command_status(host_comm_t *comm, panel_command_status_t *status);

// Reset host async state (clears any pending operation)
esp_err_t host_comm_reset(host_comm_t *comm);

const char* host_comm_get_transport_name(host_comm_t *comm);

#ifdef __cplusplus
}
#endif

#endif // HOST_COMM_H
