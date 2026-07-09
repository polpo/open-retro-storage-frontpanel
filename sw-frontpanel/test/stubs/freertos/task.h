// SPDX-License-Identifier: GPL-2.0-only
//
//  Host-test stub for ESP-IDF <freertos/task.h>. vTaskDelay is a no-op, so
//  poll loops spin through their iteration budget instantly (used to exercise
//  the timeout path without real waiting).

#ifndef HOSTTEST_FREERTOS_TASK_H
#define HOSTTEST_FREERTOS_TASK_H

#include "freertos/FreeRTOS.h"

static inline void vTaskDelay(TickType_t ticks) {
    (void)ticks;
}

#endif // HOSTTEST_FREERTOS_TASK_H
