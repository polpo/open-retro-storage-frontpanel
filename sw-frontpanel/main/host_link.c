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

#include "host_link.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "gpio_pins.h"
#include "transport_config.h"

static const char *TAG = "host_link";

#define HOST_LINK_NVS_NAMESPACE  "host_link"
#define HOST_LINK_NVS_KEY_TYPE   "transport"

void host_link_build_config(transport_type_t type, transport_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->type = type;
    cfg->timeout_ms = HOST_TIMEOUT_MS;

    if (type == TRANSPORT_TYPE_I2C) {
        cfg->device_addr = HOST_I2C_DEVICE_ADDR;
        cfg->sda_miso = PIN_SDA;
        cfg->scl_clk = PIN_SCL;
        cfg->clock_speed = HOST_I2C_CLOCK_SPEED;
    } else {
        cfg->sda_miso = PIN_SPI_MISO;
        cfg->scl_clk = PIN_SPI_CLK;
        cfg->mosi = PIN_SPI_MOSI;
        cfg->cs = PIN_HOST_CS;
        cfg->clock_speed = HOST_SPI_CLOCK_SPEED;
    }
}

bool host_link_load_type(transport_type_t *type) {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(HOST_LINK_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "No saved host transport");
        return false;
    }

    uint8_t stored = 0;
    ret = nvs_get_u8(handle, HOST_LINK_NVS_KEY_TYPE, &stored);
    nvs_close(handle);
    if (ret != ESP_OK || stored > (uint8_t)TRANSPORT_TYPE_I2C) {
        return false;
    }

    *type = (transport_type_t)stored;
    return true;
}

esp_err_t host_link_save_type(transport_type_t type) {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(HOST_LINK_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for host link: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t stored;
    if (nvs_get_u8(handle, HOST_LINK_NVS_KEY_TYPE, &stored) == ESP_OK &&
        stored == (uint8_t)type) {
        nvs_close(handle);
        return ESP_OK;
    }

    ret = nvs_set_u8(handle, HOST_LINK_NVS_KEY_TYPE, (uint8_t)type);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Saved host transport: %s",
                 type == TRANSPORT_TYPE_I2C ? "I2C" : "SPI");
    } else {
        ESP_LOGW(TAG, "Failed to save host transport: %s", esp_err_to_name(ret));
    }
    return ret;
}
