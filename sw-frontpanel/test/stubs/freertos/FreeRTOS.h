// SPDX-License-Identifier: GPL-2.0-only
//
//  Host-test stub for ESP-IDF <freertos/FreeRTOS.h>. Single-threaded host
//  tests don't need a real scheduler; provide the handful of types and macros
//  the protocol code references.

#ifndef HOSTTEST_FREERTOS_H
#define HOSTTEST_FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
typedef int      BaseType_t;

#define pdTRUE        1
#define pdFALSE       0
#define pdPASS        1
#define pdFAIL        0

#define portMAX_DELAY ((TickType_t)0xffffffffUL)

// Host tests run with no real tick; treat 1 ms as 1 tick.
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#endif // HOSTTEST_FREERTOS_H
