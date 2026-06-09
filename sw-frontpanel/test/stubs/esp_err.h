// SPDX-License-Identifier: GPL-2.0-only
//
//  Host-test stub for ESP-IDF <esp_err.h>.
//
//  Provides just enough of the esp_err_t surface for the front-panel protocol
//  code to compile and link on the host. Numeric values match ESP-IDF so that
//  assertions on returned error codes mean the same thing they do on-device.

#ifndef HOSTTEST_ESP_ERR_H
#define HOSTTEST_ESP_ERR_H

#include <stdint.h>
#include <stddef.h>  // real esp_err.h transitively provides size_t to consumers

typedef int esp_err_t;

#define ESP_OK                   0
#define ESP_FAIL                 -1

#define ESP_ERR_NO_MEM           0x101
#define ESP_ERR_INVALID_ARG      0x102
#define ESP_ERR_INVALID_STATE    0x103
#define ESP_ERR_INVALID_SIZE     0x104
#define ESP_ERR_NOT_FOUND        0x105
#define ESP_ERR_NOT_SUPPORTED    0x106
#define ESP_ERR_TIMEOUT          0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_INVALID_CRC      0x109

// Best-effort name lookup; only the codes the protocol code actually logs.
static inline const char *esp_err_to_name(esp_err_t e) {
    switch (e) {
        case ESP_OK:                   return "ESP_OK";
        case ESP_FAIL:                 return "ESP_FAIL";
        case ESP_ERR_NO_MEM:           return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_ARG:      return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE:    return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_INVALID_SIZE:     return "ESP_ERR_INVALID_SIZE";
        case ESP_ERR_NOT_FOUND:        return "ESP_ERR_NOT_FOUND";
        case ESP_ERR_TIMEOUT:          return "ESP_ERR_TIMEOUT";
        case ESP_ERR_INVALID_RESPONSE: return "ESP_ERR_INVALID_RESPONSE";
        default:                       return "ESP_ERR_?";
    }
}

#endif // HOSTTEST_ESP_ERR_H
