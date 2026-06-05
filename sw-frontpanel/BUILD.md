# Building the firmware

The front-panel firmware targets the ESP32-C3 and is built with ESP-IDF
(v5.5.2). It ships as two product variants — **PicoIDE** and **BlueSCSI** —
selected at configure time via ESP-IDF `sdkconfig` defaults. There is no
interactive `menuconfig` step required to pick the product.

## Development environment

On NixOS / with Nix, a dev shell pins ESP-IDF and the web-asset build
dependencies. Prefix the build commands below with `nix develop --command`,
or enter the shell first:

```bash
nix develop          # flake-based (preferred)
# or: nix-shell       # shell.nix
```

Without Nix, follow the upstream
[ESP-IDF toolchain setup](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/get-started/)
and run the same `idf.py` commands.

## Selecting a product

The product is chosen by layering a per-product defaults file on top of the
common `sdkconfig.defaults`:

| Product  | Defaults file                | Firmware output            |
| -------- | ---------------------------- | -------------------------- |
| PicoIDE  | `sdkconfig.defaults.picoide` | `picoide-frontpanel.bin`   |
| BlueSCSI | `sdkconfig.defaults.bluescsi`| `bluescsi-frontpanel.bin`  |

Build each variant into its **own build directory** (`-B build-<product>`) so
their `sdkconfig`s never mix. This matches what CI does.

### BlueSCSI

```bash
idf.py -B build-bluescsi \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.bluescsi" \
  build
```

Output: `build-bluescsi/bluescsi-frontpanel.bin`

### PicoIDE

```bash
idf.py -B build-picoide \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.picoide" \
  build
```

Output: `build-picoide/picoide-frontpanel.bin`

> The `-DSDKCONFIG_DEFAULTS=...` argument only needs to be passed on the first
> configure of a build directory; it is cached for subsequent `idf.py` runs in
> that directory. Mixing products in a single `build/` directory leaves a stale
> `sdkconfig` and mismatched output names — use the per-product `-B` dirs above.

## Flashing and monitoring

Replace the port to match your board (e.g. `/dev/ttyACM0`, `/dev/ttyUSB0`):

```bash
idf.py -B build-bluescsi -p /dev/ttyACM0 flash monitor   # BlueSCSI
idf.py -B build-picoide  -p /dev/ttyACM0 flash monitor   # PicoIDE
```

`flash` (re)builds as needed, writes the image, and `monitor` attaches to the
UART. Exit the monitor with `Ctrl-]`.

## Cleaning

```bash
idf.py -B build-bluescsi fullclean    # clean one product's build dir
rm -rf build-bluescsi build-picoide   # or just remove the dirs
```

Run `fullclean` if you change ESP-IDF versions — a build directory configured
against a different IDF Python is rejected on the next build.
