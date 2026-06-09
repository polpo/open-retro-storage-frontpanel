// SPDX-License-Identifier: GPL-2.0-only
//
//  Recording mock transport for host tests.
//
//  It supplies the `transport_spi_ops` / `transport_i2c_ops` symbols that the
//  real transport.c factory (transport_get_ops) returns, so host_comm calls
//  route through the real routing layer into this mock. Every two-phase
//  transaction is recorded for inspection, and poll-for-result responses are
//  scripted by the test.

#ifndef MOCK_TRANSPORT_H
#define MOCK_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "panel_protocol_defs.h"

#define MOCK_MAX_XFERS 16

// One recorded two_phase_transaction call (the "issue" half of a command).
typedef struct {
    uint8_t  command;
    uint16_t argument;
    size_t   write_len;
    size_t   read_len;
    uint8_t  write_data[PANEL_PROTOCOL_MAX_PAYLOAD];
} mock_xfer_t;

// Reset all recorded state and scripted responses. Call at the start of each test.
void mock_reset(void);

// Inspect recorded two_phase_transaction calls.
int                mock_xfer_count(void);
const mock_xfer_t *mock_xfer(int index);       // NULL if out of range
const mock_xfer_t *mock_last_xfer(void);        // NULL if none recorded

// --- Scripting the transport's behaviour ---

// Make the next two_phase_transaction return `err` instead of ESP_OK.
// (One-shot: cleared after it fires.)
void mock_set_xfer_error(esp_err_t err);

// Program the poll-for-result response: the async op reports ready with the
// given result bytes (copied back to the caller, truncated to its buffer).
void mock_set_poll_result(const void *data, uint16_t size);

// Program an empty ready response (size 0) — for fire-and-forget commands.
void mock_set_poll_ready_empty(void);

// Poll never reports ready -> exercises host_comm's timeout path.
void mock_set_poll_never_ready(void);

// Poll reports the async error state -> transport returns ESP_FAIL.
void mock_set_poll_error(void);

#endif // MOCK_TRANSPORT_H
