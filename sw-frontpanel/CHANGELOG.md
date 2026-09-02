# v0.5.0

- One BlueSCSI firmware image for all boards: the panel auto-detects the host
  transport (v2 = I2C, Ultra/Ultra Wide = SPI) at boot and remembers it, so
  `bluescsi-frontpanel.bin` replaces the separate `-ultra`/`-v2` images
- Recover the OLED when it misses power-on init (slow 3V3 ramp on host power)
- Show the SD-card panel update path in the BlueSCSI web UI

# v0.4.0

- Add screensavers and configurable idle/blank timeouts
- Combine main board & front panel firmware updates into one operation
- Better reporting of SD card state from main board
- Create releases

# v0.3.0

- Config file editor component
- Better status bar
- Layout improvements
- WiFi status display improvements
- Minification/gzip of static assets for web UI
- Main board comms reliability improvements

# v0.2.2

- Fix display glitch on firmware update screen

# v0.2.1

- Fix menu scrolling on menus that barely go off the screen

# v0.2.0

- Better display of main/front panel FW versions
- Fix for keypress to wake up display passing through as event
- Better handling of image status, handle fixed HDDs on status screen

# v0.1.0

- Initial beta release
