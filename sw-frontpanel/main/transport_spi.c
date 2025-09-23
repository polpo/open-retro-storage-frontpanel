#include "transport.h"
#include "panel_protocol_defs.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
/* #define LOG_LOCAL_LEVEL ESP_LOG_NONE */
#include "esp_log.h"
#include <string.h>

static const char *TAG = "transport_spi";

#define SPI_HOST    SPI2_HOST  // Must match was instantiated in main.c

typedef struct {
    spi_device_handle_t spi_device;
    bool owns_bus;  // Track if we initialized the bus or are sharing it
    /* uint8_t dummy_tx_buffer[PANEL_PROTOCOL_MAX_PAYLOAD];  // Static buffer for read operations */
} transport_spi_priv_t;

static esp_err_t transport_spi_init(transport_handle_t *handle) {
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    // Allocate private data
    transport_spi_priv_t *priv = calloc(1, sizeof(transport_spi_priv_t));
    if (!priv) {
        return ESP_ERR_NO_MEM;
    }
    
    handle->priv = priv;
    priv->owns_bus = false;

    // Initialize dummy buffer with zeros for read operations
    /* memset(priv->dummy_tx_buffer, 0, sizeof(priv->dummy_tx_buffer)); */

    // Try to add device to existing SPI bus (initialized in main.c)
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = handle->config.clock_speed,
        .mode = 1,  // SPI mode 1 (CPOL=0, CPHA=1) - the Pico seems to do better in mode 1
        .spics_io_num = handle->config.cs,
        .queue_size = 7,
        .flags = 0,
        .cs_ena_pretrans = 2,   // Keep CS active for 2 cycles before transaction
        .cs_ena_posttrans = 2,  // Keep CS active for 2 cycles after transaction
        .pre_cb = NULL,
        .post_cb = NULL,
    };

    // Try to add device to potentially existing bus
    esp_err_t ret = spi_bus_add_device(SPI_HOST, &dev_cfg, &priv->spi_device);
    
    if (ret == ESP_ERR_INVALID_STATE) {
        // Bus not initialized yet - this shouldn't happen since main.c initializes it
        // but keeping this as fallback
        ESP_LOGI(TAG, "SPI bus not initialized, initializing it ourselves (fallback)");
        
        spi_bus_config_t bus_cfg = {
            .miso_io_num = handle->config.sda_miso,  // MISO pin
            .mosi_io_num = handle->config.mosi,       // MOSI pin
            .sclk_io_num = handle->config.scl_clk,    // CLK pin
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4096,
        };

        // Initialize SPI bus
        ret = spi_bus_initialize(SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
            free(priv);
            handle->priv = NULL;
            return ret;
        }
        
        priv->owns_bus = true;
        
        // Now add the device
        ret = spi_bus_add_device(SPI_HOST, &dev_cfg, &priv->spi_device);
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
        if (priv->owns_bus) {
            spi_bus_free(SPI_HOST);
        }
        free(priv);
        handle->priv = NULL;
        return ret;
    }

    handle->initialized = true;
    ESP_LOGI(TAG, "SPI transport initialized (MISO: %d, MOSI: %d, CLK: %d, CS: %d) %s",
             handle->config.sda_miso, handle->config.mosi, 
             handle->config.scl_clk, handle->config.cs,
             priv->owns_bus ? "[owns bus]" : "[sharing bus]");
    
    return ESP_OK;
}

static esp_err_t transport_spi_deinit(transport_handle_t *handle) {
    if (!handle || !handle->priv) {
        return ESP_ERR_INVALID_ARG;
    }

    transport_spi_priv_t *priv = (transport_spi_priv_t *)handle->priv;
    
    // Remove our device from the bus
    spi_bus_remove_device(priv->spi_device);
    
    // Only free the bus if we initialized it
    if (priv->owns_bus) {
        spi_bus_free(SPI_HOST);
        ESP_LOGI(TAG, "SPI bus freed");
    }
    
    free(priv);
    handle->priv = NULL;
    handle->initialized = false;
    
    ESP_LOGI(TAG, "SPI transport deinitialized");
    return ESP_OK;
}


static const char* transport_spi_get_name(void) {
    return "SPI";
}

// New two-phase protocol functions
static esp_err_t transport_spi_send_header(transport_handle_t *handle, uint8_t command, uint16_t argument, uint16_t payload_size) {
    if (!handle || !handle->priv) {
        return ESP_ERR_INVALID_ARG;
    }

    transport_spi_priv_t *priv = (transport_spi_priv_t *)handle->priv;

    panel_protocol_header_t header = {
        .command = command,
        .argument = argument,
        .payload_size = payload_size
    };

    ESP_LOGI(TAG, "Header bytes: %02x %02x %02x %02x %02x",
             header.command,
             (uint8_t)(header.argument & 0xFF),
             (uint8_t)((header.argument >> 8) & 0xFF),
             (uint8_t)(header.payload_size & 0xFF),
             (uint8_t)((header.payload_size >> 8) & 0xFF));

    spi_transaction_t trans = {
        .length = PANEL_PROTOCOL_HEADER_SIZE * 8,  // Length in bits
        .tx_buffer = &header,
        .rx_buffer = NULL,
    };

    esp_err_t ret = spi_device_transmit(priv->spi_device, &trans);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI header send failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Header sent successfully");
    }

    return ret;
}

static esp_err_t transport_spi_send_payload(transport_handle_t *handle, const uint8_t *data, size_t len) {
    if (!handle || !handle->priv || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    transport_spi_priv_t *priv = (transport_spi_priv_t *)handle->priv;

    spi_transaction_t trans = {
        .length = len * 8,  // Length in bits
        .tx_buffer = data,
        .rx_buffer = NULL,
    };

    ESP_LOGI(TAG, "Sending payload: %zu bytes", len);
    esp_err_t ret = spi_device_transmit(priv->spi_device, &trans);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI payload send failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Payload sent successfully");
    }

    return ret;
}

static esp_err_t transport_spi_read_payload(transport_handle_t *handle, uint8_t *data, size_t len) {
    if (!handle || !handle->priv || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    transport_spi_priv_t *priv = (transport_spi_priv_t *)handle->priv;

    // Check if length exceeds our dummy buffer size
    /* if (len > sizeof(priv->dummy_tx_buffer)) { */
    /*     ESP_LOGE(TAG, "Payload read length %zu exceeds dummy buffer size %zu", len, sizeof(priv->dummy_tx_buffer)); */
    /*     return ESP_ERR_INVALID_SIZE; */
    /* } */

    ESP_LOGI(TAG, "Reading payload: %zu bytes", len);
    spi_transaction_t trans = {
        .length = len * 8,  // Length in bits
        .tx_buffer = NULL, //priv->dummy_tx_buffer,  // Use static buffer
        .rx_buffer = data,
    };

    esp_err_t ret = spi_device_transmit(priv->spi_device, &trans);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI payload read failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Payload read successfully");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, (len > 32) ? 32 : len, ESP_LOG_INFO);
    }

    return ret;
}

// High-level two-phase transaction function
esp_err_t transport_spi_two_phase_transaction(transport_handle_t *handle,
                                              uint8_t command, uint16_t argument,
                                              const uint8_t *write_data, size_t write_len,
                                              uint8_t *read_data, size_t read_len) {
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t payload_size = 0;
    bool is_read = PANEL_CMD_IS_READ(command);

    if (is_read && read_len > 0) {
        payload_size = read_len;
    } else if (!is_read && write_data && write_len > 0) {
        payload_size = write_len;
    }

    ESP_LOGI(TAG, "Two-phase transaction: cmd=0x%02x, arg=0x%04x, payload_size=%u, is_read=%d",
             command, argument, payload_size, is_read);

    // Phase 1: Send header
    ESP_LOGI(TAG, "Phase 1: Sending header (5 bytes)");
    esp_err_t ret = transport_spi_send_header(handle, command, argument, payload_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Phase 1 failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Phase 1 complete");

    // Phase 2: Transfer payload (if any)
    if (payload_size > 0) {
        if (is_read) {
            // Read operation
            ESP_LOGI(TAG, "Phase 2: Reading payload (%u bytes)", payload_size);
            ret = transport_spi_read_payload(handle, read_data, payload_size);
            if (ret == ESP_OK) {
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, read_data, (payload_size > 32) ? 32 : payload_size, ESP_LOG_INFO);
            }
        } else {
            // Write operation
            ESP_LOGI(TAG, "Phase 2: Writing payload (%u bytes)", payload_size);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, write_data, (write_len > 32) ? 32 : write_len, ESP_LOG_INFO);
            ret = transport_spi_send_payload(handle, write_data, write_len);
        }
    }

    return ret;
}

esp_err_t transport_spi_poll_async_status(transport_handle_t *handle,
                                          bool *ready, uint16_t *response_size,
                                          uint8_t *result_data, size_t max_result_size) {
    if (!handle || !ready || !response_size) {
        return ESP_ERR_INVALID_ARG;
    }

    // Read status response using the structured type
    panel_status_response_t status;
    esp_err_t ret = transport_spi_two_phase_transaction(handle,
                                                       PANEL_CMD_POLL_OP_READY, PANEL_ARG_IGNORED,
                                                       NULL, 0,
                                                       (uint8_t*)&status, sizeof(status));
    if (ret != ESP_OK) {
        return ret;
    }
    if (status.ready_flag == PANEL_ASYNC_ERROR) {
        return ESP_FAIL;
    }

    *ready = (status.ready_flag == PANEL_ASYNC_READY);
    *response_size = status.response_size;
    ESP_LOGI(TAG, "Ready: %u", status.ready_flag);

    // If ready and result data requested, read the result data separately
    if (*ready && *response_size > 0 && result_data && max_result_size > 0) {
        size_t read_size = (*response_size <= max_result_size) ? *response_size : max_result_size;
        ret = transport_spi_read_payload(handle, result_data, read_size);
    }

    return ret;
}

// SPI transport operations
const transport_ops_t transport_spi_ops = {
    .init = transport_spi_init,
    .deinit = transport_spi_deinit,
    .two_phase_transaction = transport_spi_two_phase_transaction,
    .poll_async_status = transport_spi_poll_async_status,
    .get_name = transport_spi_get_name,
};
