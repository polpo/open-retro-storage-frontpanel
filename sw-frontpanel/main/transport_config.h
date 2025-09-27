#ifndef TRANSPORT_CONFIG_H
#define TRANSPORT_CONFIG_H

// Transport selection - comment/uncomment to select transport type
// If neither is defined, I2C will be used as default

#define TRANSPORT_USE_SPI    // Use SPI transport for host communication
// #define TRANSPORT_USE_I2C    // Use I2C transport for host communication (default)

// If no transport is explicitly selected, default to I2C
#if !defined(TRANSPORT_USE_SPI) && !defined(TRANSPORT_USE_I2C)
#define TRANSPORT_USE_I2C
#endif

// Ensure only one transport is selected
#if defined(TRANSPORT_USE_SPI) && defined(TRANSPORT_USE_I2C)
#error "Only one transport type can be selected. Please define either TRANSPORT_USE_SPI or TRANSPORT_USE_I2C, not both."
#endif

// Host communication settings
#define HOST_TIMEOUT_MS     1000       // Communication timeout in milliseconds

// I2C-specific settings
#ifdef TRANSPORT_USE_I2C
#define HOST_DEVICE_ADDR    0x50       // I2C address or SPI device ID
#define HOST_CLOCK_SPEED    400000     // 400 kHz for I2C
#endif

// SPI-specific settings
#ifdef TRANSPORT_USE_SPI
#define HOST_DEVICE_ADDR    -1         // SPI has no device addr
#define SPI_MODE            0          // SPI mode (0-3)
#define HOST_CLOCK_SPEED    10000000    // 5MHz for SPI
#define SPI_MAX_TRANSFER    4096       // Maximum transfer size in bytes
#endif

#endif // TRANSPORT_CONFIG_H
