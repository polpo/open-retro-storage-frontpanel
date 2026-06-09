// SPDX-License-Identifier: GPL-2.0-only
//
//  Host-test stub for ESP-IDF <freertos/semphr.h>. The mutex is a no-op:
//  host tests are single-threaded, so take/give always succeed. A non-NULL
//  sentinel handle is returned so callers' null checks behave normally.

#ifndef HOSTTEST_FREERTOS_SEMPHR_H
#define HOSTTEST_FREERTOS_SEMPHR_H

#include "freertos/FreeRTOS.h"

typedef void *SemaphoreHandle_t;

#define HOSTTEST_MUTEX_SENTINEL ((SemaphoreHandle_t)(void *)0x1)

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    return HOSTTEST_MUTEX_SENTINEL;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) {
    (void)s;
    (void)t;
    return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    (void)s;
    return pdTRUE;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t s) {
    (void)s;
}

#endif // HOSTTEST_FREERTOS_SEMPHR_H
