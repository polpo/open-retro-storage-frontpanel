// SPDX-License-Identifier: GPL-2.0-only
//
//  Recording mock transport. See mock_transport.h.

#include "mock_transport.h"
#include "transport.h"
#include <string.h>

// --- Recorded transactions ---------------------------------------------------

static mock_xfer_t s_xfers[MOCK_MAX_XFERS];
static int         s_xfer_count;

// --- Scripted responses ------------------------------------------------------

static esp_err_t s_xfer_error;        // one-shot error for next transaction
static int       s_xfer_error_armed;

static uint8_t   s_poll_data[PANEL_PROTOCOL_MAX_PAYLOAD];
static uint16_t  s_poll_size;
static int       s_poll_ready;        // poll reports ready
static int       s_poll_error;        // poll reports async-error state

void mock_reset(void) {
    memset(s_xfers, 0, sizeof(s_xfers));
    s_xfer_count = 0;
    s_xfer_error = ESP_OK;
    s_xfer_error_armed = 0;
    memset(s_poll_data, 0, sizeof(s_poll_data));
    s_poll_size = 0;
    s_poll_ready = 1;     // default: async op completes immediately, empty result
    s_poll_error = 0;
}

int mock_xfer_count(void) { return s_xfer_count; }

const mock_xfer_t *mock_xfer(int index) {
    if (index < 0 || index >= s_xfer_count) return NULL;
    return &s_xfers[index];
}

const mock_xfer_t *mock_last_xfer(void) {
    if (s_xfer_count == 0) return NULL;
    return &s_xfers[s_xfer_count - 1];
}

void mock_set_xfer_error(esp_err_t err) {
    s_xfer_error = err;
    s_xfer_error_armed = 1;
}

void mock_set_poll_result(const void *data, uint16_t size) {
    if (size > sizeof(s_poll_data)) size = sizeof(s_poll_data);
    memcpy(s_poll_data, data, size);
    s_poll_size = size;
    s_poll_ready = 1;
    s_poll_error = 0;
}

void mock_set_poll_ready_empty(void) {
    s_poll_size = 0;
    s_poll_ready = 1;
    s_poll_error = 0;
}

void mock_set_poll_never_ready(void) {
    s_poll_ready = 0;
    s_poll_error = 0;
}

void mock_set_poll_error(void) {
    s_poll_error = 1;
}

// --- transport_ops_t implementation -----------------------------------------

static esp_err_t mock_init(transport_handle_t *handle) {
    handle->initialized = true;
    return ESP_OK;
}

static esp_err_t mock_deinit(transport_handle_t *handle) {
    handle->initialized = false;
    return ESP_OK;
}

static esp_err_t mock_two_phase(transport_handle_t *handle,
                                uint8_t command, uint16_t argument,
                                const uint8_t *write_data, size_t write_len,
                                uint8_t *read_data, size_t read_len) {
    (void)handle;

    if (s_xfer_count < MOCK_MAX_XFERS) {
        mock_xfer_t *x = &s_xfers[s_xfer_count];
        x->command   = command;
        x->argument  = argument;
        x->write_len = write_len;
        x->read_len  = read_len;
        if (write_data && write_len > 0) {
            size_t n = write_len > sizeof(x->write_data) ? sizeof(x->write_data) : write_len;
            memcpy(x->write_data, write_data, n);
        }
    }
    s_xfer_count++;

    if (s_xfer_error_armed) {
        s_xfer_error_armed = 0;
        return s_xfer_error;
    }

    // Synchronous read commands (GET_DEVICE_STATUS / GET_PLAYBACK_STATUS /
    // GET_COMMAND_STATUS) take their result in phase 2 via read_data rather
    // than via a poll. Serve it from the same scripted response buffer.
    if (read_data && read_len > 0) {
        size_t n = read_len > sizeof(s_poll_data) ? sizeof(s_poll_data) : read_len;
        memcpy(read_data, s_poll_data, n);
    }
    return ESP_OK;
}

static esp_err_t mock_poll(transport_handle_t *handle,
                           bool *ready, uint16_t *response_size,
                           uint8_t *result_data, size_t max_result_size) {
    (void)handle;

    if (s_poll_error) {
        return ESP_FAIL;
    }

    *ready = s_poll_ready ? true : false;
    *response_size = s_poll_size;

    if (*ready && s_poll_size > 0 && result_data && max_result_size > 0) {
        size_t n = s_poll_size <= max_result_size ? s_poll_size : max_result_size;
        memcpy(result_data, s_poll_data, n);
    }
    return ESP_OK;
}

static const char *mock_get_name(void) { return "MOCK"; }

#define MOCK_OPS_INITIALIZER {                  \
    .init = mock_init,                          \
    .deinit = mock_deinit,                      \
    .two_phase_transaction = mock_two_phase,    \
    .poll_async_status = mock_poll,             \
    .get_name = mock_get_name,                  \
}

// The real transport.c factory (transport_get_ops) returns one of these by
// compile-time flag. Defining both means the test links regardless of which
// TRANSPORT_USE_* the build is configured for.
const transport_ops_t transport_spi_ops = MOCK_OPS_INITIALIZER;
const transport_ops_t transport_i2c_ops = MOCK_OPS_INITIALIZER;
