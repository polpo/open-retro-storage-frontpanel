#pragma once

#include "driver/gpio.h"

// OLED Display control pins (SPI mode)
#define PIN_OLED_CS     GPIO_NUM_0   // Chip Select for SPI OLED
#define PIN_OLED_DC     GPIO_NUM_1   // Data/Command for SPI OLED

// SPI interface pins (shared by OLED display and future SPI devices)
#define PIN_SPI_MISO    GPIO_NUM_2   // Master In Slave Out (not used by OLED)
#define PIN_SPI_CLK     GPIO_NUM_6   // SPI Clock
#define PIN_SPI_MOSI    GPIO_NUM_7   // Master Out Slave In (OLED data)

// Activity indicator input (from main board)
#define PIN_ACT_IN      GPIO_NUM_20

// Navigation button pins
#define PIN_NAV_UP      GPIO_NUM_4
#define PIN_NAV_RIGHT   GPIO_NUM_9
#define PIN_NAV_DOWN    GPIO_NUM_5
#define PIN_NAV_LEFT    GPIO_NUM_8

// Host interface chip select
#define PIN_HOST_CS     GPIO_NUM_10

// LED output (WS2812B addressable LED strip)
#define PIN_LED_OUT     GPIO_NUM_18

// I2C interface pins (for communication with main board)
#define PIN_SCL         GPIO_NUM_19  // I2C Clock
#define PIN_SDA         GPIO_NUM_3  // I2C Data

// UART interface
#define PIN_UART_TX     GPIO_NUM_21

// LED strip configuration
#define LED_STRIP_GPIO_NUM  PIN_LED_OUT
