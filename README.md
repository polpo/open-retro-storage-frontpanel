# Open Retro Storage Front Panel

A physical control interface for retro storage emulator projects including [PicoIDE](https://github.com/polpo/picoide) and [BlueSCSI](https://github.com/BlueSCSI/BlueSCSI-v2).

## Overview

The Open Retro Storage Front Panel provides a user-friendly interface for browsing and selecting disc images on storage emulators without needing to access the SD card directly. It features:

- **SPI OLED Display**: 128x64 1.3" screen for menu navigation and status display
- **4-Way Navigation**: Physical buttons for browsing disc images and settings
- **WiFi Interface**: Web-based control for changing disc images, adjusting settings, uploading new images, and firmware updates
- **3.5" Drive Bay Enclosure**: Designed for internal or external-facing mounting in standard drive bays

## Project Structure

```
open-retro-storage-frontpanel/
├── sw-frontpanel/       # Firmware source code
├── hw-frontpanel-3.5/   # PCB hardware design
└── enclosure/           # 3D printable enclosure design
```

### sw-frontpanel

ESP32-C3 firmware built with the ESP-IDF SDK (v5.5.1+).

**Features:**
- SPI or I2C communication with host storage emulator
- Menu system for disc image browsing and selection
- WiFi station and access point modes with mDNS (`picoide.local`)
- Embedded web server with REST API
- OTA firmware updates with SHA256 verification
- WS2812B addressable LED support
- Auto-dim (2 min) and auto-off (10 min) display power management

**Build:**
```bash
cd sw-frontpanel
idf.py build
idf.py flash monitor
```

### hw-frontpanel-3.5

KiCad 8 design files for the front panel PCB.

**Main components:**
- ESP32-C3-MINI-1 module
- SH1107 1.3" OLED display (SPI interface)
- SK6812MINI-E (WS2812 compatible) addressable RGB LED
- Silicone rubber dome navigation buttons
- JST-SH connectors for QWIIC/I2C communication with host, external activity LED input, and development debugging, 0.5mm FFC connector for SPI communication with host, and Tag-Connect footprint for initial programming in production

Production files for JLCPCB assembly are included in `jlcpcb/production_files/`.

### enclosure

FreeCAD design for a 3.5" drive bay enclosure with STEP and STL exports for 3D printing.

**Included parts:**
- Main enclosure body
- Button caps
- Light pipe for LED diffusion
- Silicone button option

## Web Interface

The front panel hosts a web interface accessible at `http://picoide.local` (or via IP address) providing:

- Current disc image status
- Directory browsing
- Image selection/ejection
- WiFi network configuration
- Firmware status and updates

## Licensing

| Component | License |
|-----------|---------|
| sw-frontpanel | [GPL-2.0-only](sw-frontpanel/LICENSE) |
| hw-frontpanel-3.5 | [CERN-OHL-S-2.0](hw-frontpanel-3.5/LICENSE) |
| enclosure | [CERN-OHL-S-2.0](enclosure/LICENSE) |

## Credits

- Firmware by Ian Scott with input from Eric Helgeson.
- Board design by Ian Scott with input from Jason Merrill.
- Enclosure by Jason Merrill.

## Contributing

Contributions are welcome! Please ensure any firmware contributions maintain GPL v2 compatibility and hardware contributions are compatible with CERN-OHL-S v2.

## Building the firmware

### Install ESP-IDF and prerequesites

#### Linux and macOS

Follow the ESP-IDF [Standard Toolchain Setup for Linux and macOS](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/get-started/linux-macos-setup.html) guide.

#### Windows

Follow the ESP-IDF [Standard Setup of Toolchain for Windows](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/get-started/windows-setup.html) guide. Note this is not necessary if you are using the [VS Code extension for ESP-IDF](https://github.com/espressif/vscode-esp-idf-extension/blob/master/README.md).

### Building

If using the command line, cd to `sw-frontpanel` and run `idf.py build` to build the firmware. `idf.py flash monitor` will build and flash the firmware and connect to the UART on the ESP32 for monitoring.

If using the VS Code extension, follow the instructions in [the extension's README](https://github.com/espressif/vscode-esp-idf-extension/blob/master/README.md#using-the-esp-idf-extension-for-vs-code).

