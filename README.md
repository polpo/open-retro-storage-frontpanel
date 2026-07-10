# Open Retro Storage Front Panel

A physical control interface for retro storage emulator projects including [PicoIDE](https://github.com/polpo/picoide) and [BlueSCSI](https://github.com/BlueSCSI/BlueSCSI-v2), based on the ESP32-C3 microcontroller.

<img width="800" alt="PicoIDE variant of the enclosure" src="https://github.com/user-attachments/assets/ff9f9c1c-85b6-44e5-af8e-88a773e6bf9e" />

<img width="800" alt="BlueSCSI variant of the enclosure" src="https://github.com/user-attachments/assets/31442f82-15e1-4b8d-b3b2-79baf53444b8" />

## Overview

The Open Retro Storage Front Panel provides a user-friendly interface for browsing, selecting, and managing drive images storage emulators live on the device. It features:

- **SPI OLED Display**: 128x64 1.3" screen for menu navigation and status display
- **4-Way Navigation**: Physical buttons for browsing drive images and settings
- **WiFi Interface**: Web-based control for changing drive images, adjusting settings, uploading new images, and firmware updates
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
- Menu system for drive image browsing and selection
- WiFi client and access point modes with mDNS
- Embedded web server with REST API
- OTA firmware updates with SHA256 verification
- WS2812B addressable LED support
- Screen saver, auto-dim, or auto-off to preserve OLED display life

**Build:**
```bash
cd sw-frontpanel
idf.py build
idf.py flash monitor
```

### hw-frontpanel-3.5

KiCad design files for the front panel PCB.

<img width="40%" alt="frontpanel-top" src="https://github.com/user-attachments/assets/ed4ad325-748a-4720-aa78-37386041bb4b" /> <img width="40%" alt="frontpanel-bottom" src="https://github.com/user-attachments/assets/b04db504-06e6-4ea1-ab4e-66562adf2961" />

**Main components:**
- ESP32-C3-MINI-1 module
- SH1107 1.3" OLED display (SPI interface)
- SK6812MINI-E (WS2812 compatible) addressable RGB LED
- Navigation buttons (either 1-piece silicone rubber dome or plastic button caps w/ individual domes)
- JST-SH connectors for QWIIC/I2C communication with host, external activity LED input, and development debugging, 0.5mm FFC connector for SPI communication with host, and Tag-Connect footprint for initial programming in production

Production files for JLCPCB assembly are included in `jlcpcb/production_files/`.

### enclosure

FreeCAD design for a 3.5" drive bay enclosure with STEP and STL exports for 3D printing.

**Included parts:**
- Main enclosure body
- Button caps
- Light pipe for activity/status LED
- Silicone button option

## Web Interface

The front panel hosts a web interface accessible at `http://picoide.local` (or via IP address) providing:

- Current drive image status
- Directory browsing
- File upload/download
- Config .ini file editing
- Image selection/ejection
- Firmware status and updates

## Licensing

| Component | License |
|-----------|---------|
| sw-frontpanel | [GPL-2.0-only](sw-frontpanel/LICENSE) |
| hw-frontpanel-3.5 | [CERN-OHL-S-2.0](hw-frontpanel-3.5/LICENSE) |
| enclosure | [CERN-OHL-S-2.0](enclosure/LICENSE) |

## Credits

- Firmware by Ian Scott in collaboration with Eric Helgeson.
- Board design by Ian Scott with input from Jason Merrill.
- Enclosure by Jason Merrill.

## Open source components

- [ESP-IDF](https://github.com/espressif/esp-idf) development framework for ESP32
- [u8g2](https://github.com/olikraus/u8g2) graphics library for OLED
- [u8g2-hal-esp-idf](https://github.com/mkfrey/u8g2-hal-esp-idf) u8g2 HAL layer for ESP-IDF

## Contributing

Contributions are welcome! Please ensure any firmware contributions maintain GPL v2 compatibility and hardware contributions are compatible with CERN-OHL-S v2.

## Building the firmware

Make sure you check out this repository with submodules. If it's already checked out, `git submodule update --init` will fetch them.

### Install ESP-IDF and prerequesites

#### Linux and macOS

Follow the ESP-IDF [Standard Toolchain Setup for Linux and macOS](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/get-started/linux-macos-setup.html) guide.

#### Windows

Follow the ESP-IDF [Standard Setup of Toolchain for Windows](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/get-started/windows-setup.html) guide. Note this is not necessary if you are using the [VS Code extension for ESP-IDF](https://github.com/espressif/vscode-esp-idf-extension/blob/master/README.md).

### Configuring

Various ESP-IDF SDK config variables control how the firmware is built. You can set them with the ESP-IDF menuconfig or override them in the `sdkconfig.defaults` file.

* `sdkconfig.defaults.picoide`: set when building for PicoIDE.
* `sdkconfig.defaults.bluescsi`: set when building for BlueSCSI.
* `sdkconfig.defaults.i2c`: set when using I2C instead of SPI interface. Used with BlueSCSI v2 (non-Ultra).

### Building

If using the command line, cd to `sw-frontpanel` and run `idf.py build` to build the firmware. `idf.py flash monitor` will build and flash the firmware and connect to the UART on the ESP32 for monitoring.

If using the VS Code extension, follow the instructions in [the extension's README](https://github.com/espressif/vscode-esp-idf-extension/blob/master/README.md#using-the-esp-idf-extension-for-vs-code).

