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

// The transport is selected at runtime (transport_config_t.type), so both
// implementations' constants are defined unconditionally. Which transport a
// build actually uses is the Kconfig choice HOST_TRANSPORT: forced SPI or I2C,
// or AUTO (BlueSCSI) where the panel detects the link and persists it in NVS.

// Host communication settings
#define HOST_TIMEOUT_MS     1000       // Communication timeout in milliseconds

// I2C settings (BlueSCSI v2)
#define HOST_I2C_DEVICE_ADDR  0x50
#define HOST_I2C_CLOCK_SPEED  1000000  // 1 MHz (fast-mode-plus); needs good pull-ups
// Delay between header/write and read phases (lets the slave prepare its response).
// Busy-wait in microseconds; the slave prepares reads in its FINISH ISR, so this
// can be small. Too small -> master reads before the slave is ready.
#define I2C_INTER_PHASE_DELAY_US  200

// I2C link retries. A busy slave NACKs its address (the RP2040 slave does not
// clock-stretch), so a transient busy window surfaces as a NACK/timeout. Retry
// briefly to match SPI, where the master always clocks and readiness is handled
// by POLL_OP_READY.
#define I2C_RETRY_COUNT           3
#define I2C_RETRY_DELAY_US        200

// SPI settings (BlueSCSI Ultra/Ultra Wide, PicoIDE)
#define HOST_SPI_CLOCK_SPEED  10000000 // 10MHz (matches PicoIDE)
// Delay between header and payload phases
// No longer needed since IRQ handler debug prints are disabled by default on main board
#define SPI_INTER_PHASE_DELAY_US  0

// Startup delay: time to wait for main board to be ready before first comm attempt
#ifdef CONFIG_PRODUCT_BLUESCSI
#define HOST_STARTUP_DELAY_MS  2000
#else
#define HOST_STARTUP_DELAY_MS  1000
#endif

#endif // TRANSPORT_CONFIG_H
