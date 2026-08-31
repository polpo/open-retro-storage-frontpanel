# SPDX-License-Identifier: GPL-2.0-only
#
# Host-native test build for the front-panel firmware protocol/comm layer.
# No ESP-IDF, no hardware: compiles a subset of main/ against the stubs in
# stubs/ and a recording mock transport, then runs the suite.
#
#   make        # build + run the tests
#   make build  # build only
#   make clean
#
# Firmware sources (main/) are compiled leniently: their printf-style format
# strings are checked by ESP-IDF for the target, not the host, so host -Werror
# would flag false positives (e.g. %lu vs uint32_t). Test code is -Werror.

CC      ?= cc
MAIN    := ../main
BUILD   := obj

# -Og: some toolchains (e.g. Nix gcc) inject _FORTIFY_SOURCE, which requires
# an optimization level; -Og keeps debuggability while satisfying it.
# CONFIG_PRODUCT_BLUESCSI so the initiator-status code in host_comm.c is
# actually compiled here; nothing in host_comm.c is PicoIDE-only.
COMMON    := -std=c11 -g -Og -I. -Istubs -I$(MAIN) -DCONFIG_PRODUCT_BLUESCSI
WARN_TEST := -Wall -Wextra -Werror
# Keep -Wformat on (some toolchains force -Werror=format-security, which needs
# it); firmware uses literal format strings so that's satisfied. %lu/uint32_t
# host mismatches stay as harmless warnings since SUT isn't -Werror.
WARN_SUT  := -Wall -Wextra -Wno-unused-variable

BIN := runtests

# Firmware sources under test (real code, compiled host-side, lenient warnings).
SUT_OBJS := $(BUILD)/host_comm.o $(BUILD)/transport.o $(BUILD)/panel_policy.o

# Test scaffolding (strict warnings).
TEST_OBJS := \
	$(BUILD)/test_main.o \
	$(BUILD)/test_protocol_defs.o \
	$(BUILD)/test_host_comm.o \
	$(BUILD)/test_panel_policy.o \
	$(BUILD)/mock_transport.o \
	$(BUILD)/esp_rom_crc.o

OBJS := $(SUT_OBJS) $(TEST_OBJS)

.PHONY: all build test clean
all: test

build: $(BIN)

$(BUILD):
	mkdir -p $(BUILD)

# Firmware objects (lenient).
$(BUILD)/host_comm.o: $(MAIN)/host_comm.c | $(BUILD)
	$(CC) $(COMMON) $(WARN_SUT) -c $< -o $@
$(BUILD)/transport.o: $(MAIN)/transport.c | $(BUILD)
	$(CC) $(COMMON) $(WARN_SUT) -c $< -o $@
$(BUILD)/panel_policy.o: $(MAIN)/panel_policy.c | $(BUILD)
	$(CC) $(COMMON) $(WARN_SUT) -c $< -o $@

# Test + stub objects (strict).
$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(COMMON) $(WARN_TEST) -c $< -o $@
$(BUILD)/esp_rom_crc.o: stubs/esp_rom_crc.c | $(BUILD)
	$(CC) $(COMMON) $(WARN_TEST) -c $< -o $@

$(BIN): $(OBJS)
	$(CC) $(COMMON) $(OBJS) -o $(BIN)

test: $(BIN)
	@./$(BIN)

clean:
	rm -rf $(BUILD) $(BIN)
