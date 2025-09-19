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
    esp_err_t (*write)(transport_handle_t *handle, const uint8_t *data, size_t len);
    esp_err_t (*read)(transport_handle_t *handle, uint8_t *data, size_t len);
    esp_err_t (*write_then_read)(transport_handle_t *handle, 
                                  const uint8_t *write_data, size_t write_len,
                                  uint8_t *read_data, size_t read_len);
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
esp_err_t transport_write(transport_handle_t *handle, const uint8_t *data, size_t len);
esp_err_t transport_read(transport_handle_t *handle, uint8_t *data, size_t len);
esp_err_t transport_write_then_read(transport_handle_t *handle,
                                     const uint8_t *write_data, size_t write_len,
                                     uint8_t *read_data, size_t read_len);
const char* transport_get_name(transport_handle_t *handle);

// New protocol functions for two-phase transactions
esp_err_t transport_two_phase_transaction(transport_handle_t *handle,
                                          uint8_t command, uint8_t argument,
                                          const uint8_t *write_data, size_t write_len,
                                          uint8_t *read_data, size_t read_len);

// Transport-specific implementations
esp_err_t transport_spi_two_phase_transaction(transport_handle_t *handle,
                                              uint8_t command, uint8_t argument,
                                              const uint8_t *write_data, size_t write_len,
                                              uint8_t *read_data, size_t read_len);

esp_err_t transport_i2c_two_phase_transaction(transport_handle_t *handle,
                                              uint8_t command, uint8_t argument,
                                              const uint8_t *write_data, size_t write_len,
                                              uint8_t *read_data, size_t read_len);

// Factory function to get the appropriate transport implementation
const transport_ops_t* transport_get_ops(void);

#endif // TRANSPORT_H