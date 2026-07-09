// SPDX-License-Identifier: GPL-2.0-only
//
//  Host-test stub for ESP-IDF <esp_rom_crc.h>.
//
//  On-device this is provided by the ESP32 ROM. The host implementation
//  (esp_rom_crc.c) mirrors the same CRC-16/CCITT (poly 0x1021, MSB-first)
//  algorithm so that exercising the panel's chunk-CRC framing is meaningful.
//  The ROM implementation remains authoritative for actual host/main-board
//  interop; these host tests assert the panel's framing logic, not the wire
//  value against a live main board.

#ifndef HOSTTEST_ESP_ROM_CRC_H
#define HOSTTEST_ESP_ROM_CRC_H

#include <stdint.h>

uint16_t esp_rom_crc16_be(uint16_t crc, const uint8_t *buf, uint32_t len);

#endif // HOSTTEST_ESP_ROM_CRC_H
