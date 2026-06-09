// SPDX-License-Identifier: GPL-2.0-only
//
//  Host-test stub for ESP-IDF <esp_log.h>. Logging is compiled out so test
//  output stays clean; define HOSTTEST_VERBOSE_LOG to echo logs to stderr.

#ifndef HOSTTEST_ESP_LOG_H
#define HOSTTEST_ESP_LOG_H

// The real esp_log.h pulls in stdio; firmware code (e.g. host_comm.c's
// snprintf) relies on that transitive include.
#include <stdio.h>

#ifdef HOSTTEST_VERBOSE_LOG
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) fprintf(stderr, "D (%s) " fmt "\n", tag, ##__VA_ARGS__)
#else
#define ESP_LOGE(tag, fmt, ...) ((void)0)
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#endif

#endif // HOSTTEST_ESP_LOG_H
