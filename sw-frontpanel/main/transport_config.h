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

#ifndef TRANSPORT_CONFIG_H
#define TRANSPORT_CONFIG_H

// Transport selection is driven by the Kconfig choice HOST_TRANSPORT, set
// per-build via the sdkconfig.defaults.<variant> files. The internal
// TRANSPORT_USE_* macros below fan out from it so the rest of the transport
// layer (transport.c / transport_spi.c / transport_i2c.c) is unchanged.
//   BlueSCSI v2              -> I2C  (sdkconfig.defaults.i2c)
//   BlueSCSI Ultra / PicoIDE -> SPI  (default)
#if defined(CONFIG_HOST_TRANSPORT_I2C)
#define TRANSPORT_USE_I2C
#elif defined(CONFIG_HOST_TRANSPORT_SPI)
#define TRANSPORT_USE_SPI
#else
// Host-native unit tests compile without sdkconfig.h; default to SPI.
#define TRANSPORT_USE_SPI
#endif

// Host communication settings
#define HOST_TIMEOUT_MS     1000       // Communication timeout in milliseconds

// I2C-specific settings
#ifdef TRANSPORT_USE_I2C
#define HOST_DEVICE_ADDR    0x50       // I2C address or SPI device ID
#define HOST_CLOCK_SPEED    400000     // 400 kHz for I2C
#define I2C_MAX_TRANSFER    4096       // Maximum transfer size in bytes
// Delay between header/write and read phases (lets the slave prepare its response)
#define I2C_INTER_PHASE_DELAY_US  1000
#endif

// SPI-specific settings
#ifdef TRANSPORT_USE_SPI
#define HOST_DEVICE_ADDR    -1         // SPI has no device addr
#define SPI_MODE            0          // SPI mode (0-3)
#define HOST_CLOCK_SPEED    10000000    // 10MHz for SPI (matches PicoIDE)
#define SPI_MAX_TRANSFER    4096       // Maximum transfer size in bytes
// Delay between header and payload phases
// No longer needed since IRQ handler debug prints are disabled by default on main board
#define SPI_INTER_PHASE_DELAY_US  0
#endif

// Startup delay: time to wait for main board to be ready before first comm attempt
#ifdef CONFIG_PRODUCT_BLUESCSI
#define HOST_STARTUP_DELAY_MS  2000
#else
#define HOST_STARTUP_DELAY_MS  1000
#endif

#endif // TRANSPORT_CONFIG_H
