#ifndef I2C_SLAVE_COMM_H
#define I2C_SLAVE_COMM_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c.h"

#define I2C_SLAVE_MAX_DATA_SIZE 256
#define I2C_SLAVE_TIMEOUT_MS 1000

// Note: This module assumes the I2C bus is already initialized by the display driver
// Both the display (0x3C) and slave device will share the same I2C bus

typedef enum {
    I2C_CMD_GET_STATUS = 0x01,
    I2C_CMD_GET_DISC_COUNT = 0x02,
    I2C_CMD_GET_DISC_LIST = 0x03,
    I2C_CMD_SELECT_DISC = 0x04,
    I2C_CMD_EJECT_DISC = 0x05,
    I2C_CMD_GET_DISC_INFO = 0x06,
    I2C_CMD_SET_CONFIG = 0x10,
    I2C_CMD_GET_CONFIG = 0x11,
    I2C_CMD_RESET = 0xFF
} i2c_command_t;

typedef enum {
    I2C_STATUS_OK = 0x00,
    I2C_STATUS_BUSY = 0x01,
    I2C_STATUS_ERROR = 0x02,
    I2C_STATUS_INVALID_CMD = 0x03,
    I2C_STATUS_NO_DISC = 0x04,
    I2C_STATUS_DISC_LOADED = 0x05
} i2c_status_t;

typedef struct {
    char name[64];
    uint32_t size;
    uint8_t type;  // 0=ISO, 1=CUE/BIN, etc.
} disc_info_t;

typedef struct {
    i2c_port_t port;
    uint8_t slave_addr;
    gpio_num_t sda_pin;
    gpio_num_t scl_pin;
    uint32_t clk_speed;
    bool initialized;
} i2c_slave_comm_t;

esp_err_t i2c_slave_comm_init(i2c_slave_comm_t *comm, i2c_port_t port, 
                              uint8_t slave_addr, gpio_num_t sda, gpio_num_t scl);
esp_err_t i2c_slave_comm_deinit(i2c_slave_comm_t *comm);

esp_err_t i2c_slave_get_status(i2c_slave_comm_t *comm, i2c_status_t *status);
esp_err_t i2c_slave_get_disc_count(i2c_slave_comm_t *comm, uint32_t *count);
esp_err_t i2c_slave_get_disc_list(i2c_slave_comm_t *comm, disc_info_t *discs, 
                                  uint32_t max_discs, uint32_t *actual_count);
esp_err_t i2c_slave_select_disc(i2c_slave_comm_t *comm, uint32_t disc_index);
esp_err_t i2c_slave_eject_disc(i2c_slave_comm_t *comm);
esp_err_t i2c_slave_get_disc_info(i2c_slave_comm_t *comm, uint32_t disc_index, 
                                  disc_info_t *info);
esp_err_t i2c_slave_send_command(i2c_slave_comm_t *comm, i2c_command_t cmd, 
                                 const uint8_t *data, size_t data_len,
                                 uint8_t *response, size_t *response_len);
esp_err_t i2c_scan_bus(i2c_slave_comm_t *comm);

#endif