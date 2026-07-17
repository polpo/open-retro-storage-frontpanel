# Open Retro Storage Front Panel

A physical control and web interface for retro storage emulator projects including [PicoIDE](https://github.com/polpo/picoide) and [BlueSCSI](https://github.com/BlueSCSI/BlueSCSI-v2), based on the ESP32-C3 microcontroller.

<img width="800" alt="PicoIDE variant of the enclosure" src="https://github.com/user-attachments/assets/ff9f9c1c-85b6-44e5-af8e-88a773e6bf9e" />

<img width="800" alt="BlueSCSI variant of the enclosure" src="https://github.com/user-attachments/assets/31442f82-15e1-4b8d-b3b2-79baf53444b8" />

## Overview

The Open Retro Storage Front Panel provides a user-friendly interface for browsing, selecting, and managing drive images for storage emulators live on the device. It features:

- **SPI or I2C (Qwiic) connection to main board**
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
- SPI or I2C (Qwiic) communication with host storage emulator
- Menu system for drive image browsing and selection
- WiFi client and access point modes with mDNS
- Embedded web server with REST API
- OTA firmware updates with SHA256 verification
- WS2812B addressable LED support
- Screen saver, auto-dim, or auto-off to preserve OLED display life

### hw-frontpanel-3.5

KiCad design files for the front panel PCB.

<img width="40%" alt="frontpanel-top" src="https://github.com/user-attachments/assets/ed4ad325-748a-4720-aa78-37386041bb4b" /> <img width="40%" alt="frontpanel-bottom" src="https://github.com/user-attachments/assets/b04db504-06e6-4ea1-ab4e-66562adf2961" />

**Main components:**
- ESP32-C3-MINI-1 module
- SH1107 1.3" OLED display (SPI interface)
- SK6812MINI-E (WS2812 compatible) addressable RGB LED
- Navigation buttons (either 1-piece silicone rubber dome or plastic button caps w/ individual domes)
- JST-SH connectors for QWIIC/I2C communication with host, external activity LED input, and development debugging, 0.5mm FFC connector for SPI communication with host, and Tag-Connect footprint for initial programming in production

Connector pinouts:

SPI (0.5mm FFC, 12 pin):

| Pin | Function |
|-----|----------|
| 1   | Activity LED in |
| 2   | GND |
| 3   | SPI CS |
| 4   | GND |
| 5   | SPI CLK |
| 6   | GND |
| 7   | SPI RX (MISO) |
| 8   | GND |
| 9   | SPI TX (MOSI) |
| 10  | GND |
| 11  | +3.3V |
| 12  | +3.3V |

I2C/Qwiic (JST-SH, 4 pin):

| Pin | Function |
|-----|----------|
| 1   | GND|
| 2   | +3.3V |
| 3   | SDA |
| 4   | SCL |

ACT (JST-SH, 2 pin):

| Pin | Function |
|-----|----------|
| 1   | Activity LED in |
| 2   | GND |

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

Make sure you check out this repository with submodules. If it's already checked out, `git submodule update --init` will fetch them. See [sw-frontpanel/BUILD.md](sw-frontpanel/BUILD.md) for detailed instructions including configuration of target type (PicoIDE/BlueSCSI, SPI/I2C).

## Programming the firmware

For normal firmware upgrades, the front panel can be upgraded via the web interface. But if you're building from scratch or need to recover a bricked unit, you'll need to program the ESP32-C3 by UART bootloader.

There are two footprints on the board that can be used to program it: J4 (prog) and J6 (dbg). J4 is a [Tag-Connect](https://www.tag-connect.com/) footprint, using the [TC2030-IDC-NL](https://www.tag-connect.com/product/tc2030-idc-nl) connector. The pinout matches the 6-pin connector on the [Espressif ESP-PROG 2](https://docs.espressif.com/projects/esp-dev-kits/en/latest/other/esp-prog-2/user_guide.html) programmer:

| Pin | Function |
|-----|----------|
| 1   | EN (RTS) |
| 2   | +3.3V |
| 3   | TX |
| 4   | GND |
| 5   | RX |
| 6   | IO0 (DTR) |

J6 is a JST SH 6-pin connector with the following pinout (pins numbered ascending left to right):

| Pin | Function |
|-----|----------|
| 1   | GND |
| 2   | RX |
| 3   | TX |
| 4   | EN (RTS) |
| 5   | IO0 (DTR) |

Note that J6 does not carry +3.3V so you'll have to provide it on the SPI or I2C (QWIIC) connector.

Note that the TX signal is shared with the LED input, so if it is driven via the SPI connector programming will not be possible.

Use `esptool` or `esp.py program` to program the firmware after it has been built.
