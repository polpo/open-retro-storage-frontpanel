# Host-native tests

Fast, hardware-free unit tests for the front-panel firmware's protocol and
host-communication layer. They compile a subset of `main/` with the host `gcc`
(no ESP-IDF, no ESP32, no QEMU) against small stubs for the ESP-IDF/FreeRTOS
APIs and a recording **mock transport**, then assert exactly what goes on the
wire.

## Running

```bash
make            # build + run (exit code = number of failed tests)
make build      # build only -> ./runtests
make clean
HOSTTEST_VERBOSE_LOG=1 ...   # (compile-time) echo firmware ESP_LOGx to stderr
```

No dependencies beyond a C11 host compiler and `make`. Also run in CI as the
`host-tests` job (`.github/workflows/build.yml`).

## What's covered

- **`test_protocol_defs.c`** — the `panel_protocol_defs.h` wire contract shared
  with the RP2350 main board: struct sizes, field offsets/packing, little-endian
  header byte layout, command-code direction/async bits, and helper macros.
- **`test_host_comm.c`** — `host_comm.c` command framing, driven through the real
  `transport.c` routing layer into the mock: which command code / argument /
  payload each call emits (signed parent-dir index, the small-vs-extended index
  branch, the `device_index` payload byte, little-endian firmware offsets,
  chunk CRC16), how responses are parsed back (big-endian counts, struct copies),
  and the error paths (timeout, transport error, oversize/!ready responses).

## How the seam works

`host_comm` reaches hardware only through the `transport_ops_t` vtable. The real
`transport.c` factory (`transport_get_ops`) returns `transport_spi_ops` /
`transport_i2c_ops` — symbols that `mock_transport.c` supplies here instead of
the real SPI/I2C drivers. So calls route through production code and land in a
mock that records each transaction and replays scripted responses. See
`mock_transport.h` for the scripting API (`mock_set_poll_result`,
`mock_set_xfer_error`, `mock_last_xfer`, …).

## Layout

```
test/
  Makefile               # split flags: firmware lenient, test code -Werror
  test_framework.h       # ~40-line assert/runner (TEST / RUN / CHECK_*)
  test_main.c            # runner; exit code = failed test count
  test_protocol_defs.c   # tier 1: header contract
  test_host_comm.c       # tier 2: command framing via mock
  mock_transport.{h,c}   # recording transport (provides *_ops symbols)
  stubs/                 # minimal host stand-ins for ESP-IDF / FreeRTOS headers
    esp_err.h  esp_log.h  esp_rom_crc.{h,c}
    driver/gpio.h  freertos/{FreeRTOS,semphr,task}.h
```

## Adding a test

1. Write `TEST(my_case) { ... CHECK_*(...); }` in the relevant file.
2. Add `RUN(my_case);` to that file's `run_*_suite()`.
3. New source file → add it to `TEST_OBJS` in the `Makefile` and declare its
   `run_*_suite()` in `test_main.c`.

To cover another `host_comm` call, script the response with `mock_set_poll_result`
(async result) or rely on the default empty-ready, invoke the function, then
assert on `mock_last_xfer()` / `mock_xfer(i)`.

## Scope / caveats

- These test the **panel side** framing and parsing, not a live main board. The
  host `esp_rom_crc16_be` mirrors the CCITT algorithm so chunk-CRC framing is
  exercised, but the ESP32 ROM implementation is authoritative on-device.
- The real SPI/I2C byte transfer (`transport_spi.c` `spi_device_transmit`, DMA,
  the ISR header/payload race) is **not** covered here — that needs on-target or
  QEMU tests. The struct/packing tests do pin the header bytes those drivers move.
