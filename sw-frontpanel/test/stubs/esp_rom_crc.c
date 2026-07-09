// SPDX-License-Identifier: GPL-2.0-only
//
//  Host implementation of esp_rom_crc16_be(). See esp_rom_crc.h for scope.
//  CRC-16/CCITT, polynomial 0x1021, processed MSB-first (the "big-endian"
//  bit order the ESP ROM "_be" variant uses), no input/output reflection.
//  Deterministic so the panel's chunk-CRC framing can be asserted on host.

#include "esp_rom_crc.h"

uint16_t esp_rom_crc16_be(uint16_t crc, const uint8_t *buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x8000) {
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}
