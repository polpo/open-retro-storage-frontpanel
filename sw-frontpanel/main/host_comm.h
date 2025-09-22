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

// Status codes for disc operations
typedef enum {
    HOST_STATUS_NO_DISC = 0x00,     // No disc loaded
    HOST_STATUS_DISC_LOADED = 0x01, // Disc loaded and ready
    HOST_STATUS_LOADING = 0x02,     // Disc loading in progress
    HOST_STATUS_ERROR = 0x03        // Disc error
} host_status_t;

// Disc information structure
typedef struct {
    char name[64];
    uint32_t size;
    uint32_t tracks;
} disc_info_t;

// Host communication handle
typedef struct {
    transport_handle_t transport;
    bool initialized;
} host_comm_t;

// Public API
esp_err_t host_comm_init(host_comm_t *comm, const transport_config_t *config);
esp_err_t host_comm_deinit(host_comm_t *comm);

esp_err_t host_comm_get_status(host_comm_t *comm, host_status_t *status);
esp_err_t host_comm_get_disc_count(host_comm_t *comm, uint32_t *count);
esp_err_t host_comm_get_disc_list(host_comm_t *comm, disc_info_t *discs, 
                                  size_t max_discs, size_t *disc_count);
esp_err_t host_comm_select_disc(host_comm_t *comm, uint32_t disc_index);
esp_err_t host_comm_eject_disc(host_comm_t *comm);
esp_err_t host_comm_get_disc_info(host_comm_t *comm, uint32_t disc_index,
                                   disc_info_t *info);

// Firmware update functions
esp_err_t host_comm_check_firmware(host_comm_t *comm);
esp_err_t host_comm_get_firmware_info(host_comm_t *comm, panel_firmware_info_t *info);
esp_err_t host_comm_read_firmware_chunk(host_comm_t *comm, uint32_t offset,
                                        uint8_t *buffer, size_t size);

// File upload functions
esp_err_t host_comm_start_file_upload(host_comm_t *comm, const char *filename, uint32_t file_size, const uint8_t *expected_hash);
esp_err_t host_comm_write_file_chunk(host_comm_t *comm, const uint8_t *data, size_t size);
esp_err_t host_comm_finish_file_upload(host_comm_t *comm, uint8_t *result_code);

const char* host_comm_get_transport_name(host_comm_t *comm);

#ifdef __cplusplus
}
#endif

#endif // HOST_COMM_H