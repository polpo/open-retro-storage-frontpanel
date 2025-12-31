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

#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "transport_config.h"

// Forward declaration
typedef struct transport_handle transport_handle_t;

// Transport operations structure
typedef struct {
    esp_err_t (*init)(transport_handle_t *handle);
    esp_err_t (*deinit)(transport_handle_t *handle);
    esp_err_t (*two_phase_transaction)(transport_handle_t *handle, 
               uint8_t command, uint16_t argument,
               const uint8_t *write_data, size_t write_len,
               uint8_t *read_data, size_t read_len);
    esp_err_t (*poll_async_status)(transport_handle_t *handle,
                                   bool *ready, uint16_t *response_size,
                                   uint8_t *result_data, size_t max_result_size);
    const char* (*get_name)(void);
} transport_ops_t;

// Transport configuration
typedef struct {
    uint8_t device_addr;  // I2C address or SPI device ID
    gpio_num_t sda_miso;  // I2C SDA or SPI MISO
    gpio_num_t scl_clk;   // I2C SCL or SPI CLK
    gpio_num_t cs;        // SPI CS (ignored for I2C)
    gpio_num_t mosi;      // SPI MOSI (ignored for I2C)
    uint32_t clock_speed; // Clock speed in Hz
    uint32_t timeout_ms;  // Operation timeout
} transport_config_t;

// Transport handle structure
struct transport_handle {
    const transport_ops_t *ops;
    transport_config_t config;
    void *priv;  // Private data for specific transport implementation
    bool initialized;
};

// Public API functions
esp_err_t transport_init(transport_handle_t *handle, const transport_config_t *config);
esp_err_t transport_deinit(transport_handle_t *handle);
esp_err_t transport_two_phase_transaction(transport_handle_t *handle,
                                          uint8_t command, uint16_t argument,
                                          const uint8_t *write_data, size_t write_len,
                                          uint8_t *read_data, size_t read_len);
esp_err_t transport_poll_async_status(transport_handle_t *handle,
                                      bool *ready, uint16_t *response_size,
                                      uint8_t *result_data, size_t max_result_size);
const char* transport_get_name(transport_handle_t *handle);

// Factory function to get the appropriate transport implementation
const transport_ops_t* transport_get_ops(void);

#endif // TRANSPORT_H
