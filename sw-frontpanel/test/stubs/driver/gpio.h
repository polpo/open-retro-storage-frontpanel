// SPDX-License-Identifier: GPL-2.0-only
//
//  Host-test stub for ESP-IDF <driver/gpio.h>. transport.h only needs the
//  gpio_num_t type for its config struct.

#ifndef HOSTTEST_DRIVER_GPIO_H
#define HOSTTEST_DRIVER_GPIO_H

typedef int gpio_num_t;

#define GPIO_NUM_NC (-1)

#endif // HOSTTEST_DRIVER_GPIO_H
