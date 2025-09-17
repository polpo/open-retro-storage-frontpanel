#ifndef HOST_COMM_H
#define HOST_COMM_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "transport.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOST_COMM_MAX_DATA_SIZE 256
#define HOST_COMM_TIMEOUT_MS 1000

// Command definitions for host communication protocol
typedef enum {
    HOST_CMD_GET_STATUS = 0x01,
    HOST_CMD_GET_DISC_COUNT = 0x02,
    HOST_CMD_GET_DISC_LIST = 0x03,
    HOST_CMD_SELECT_DISC = 0x04,
    HOST_CMD_EJECT_DISC = 0x05,
    HOST_CMD_GET_DISC_INFO = 0x06,
    HOST_CMD_SET_CONFIG = 0x10,
    HOST_CMD_GET_CONFIG = 0x11,
    HOST_CMD_RESET = 0xFF
} host_command_t;

// Status codes
typedef enum {
    HOST_STATUS_NO_CMD = 0x00,      // No command pending
    HOST_STATUS_BUSY = 0x01,        // Command processing
    HOST_STATUS_READY = 0x02,       // Response ready
    HOST_STATUS_ERROR = 0x03,       // Command failed
    HOST_STATUS_INVALID_CMD = 0x04  // Invalid command
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
esp_err_t host_comm_send_command(host_comm_t *comm, host_command_t cmd, 
                                 const uint8_t *data, size_t data_len,
                                 uint8_t *response, size_t *response_len);

const char* host_comm_get_transport_name(host_comm_t *comm);

#ifdef __cplusplus
}
#endif

#endif // HOST_COMM_H