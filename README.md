# OpenTherm wall thermostat

A DIY wall thermostat built on a **Guition JC4880P443C** 4.3" touch panel (ESP32-P4 + ESP32-C6),
a **DIYLESS Master OpenTherm Shield** and an **SHT40** room sensor, with Home Assistant integration.

Status: version 0.1 in development on `main`. The bench-tested test firmware (v2) is kept on the
[`testing-v2`](https://github.com/nthnghu/opentherm-thermostat/tree/testing-v2) branch.

Test firmware state: Everything except the boiler is wired and verified on the bench; the
OpenTherm side runs against a built-in simulated boiler until the real boiler is connected.

## Repository layout

| Folder | What it is |
|---|---|
| [`test-firmware-v2/`](test-firmware-v2/) | Current firmware (ESP-IDF 5.5 + LVGL 9). See its [README](test-firmware-v2/README.md). |
| [`c6-coprocessor/`](c6-coprocessor/) | ESP-Hosted 3.1.0 coprocessor firmware for the on-board ESP32-C6 (Wi-Fi). |
| [`test-firmware/`](test-firmware/) | First hardware test firmware (display, touch, audio, Wi-Fi). Kept for reference. |

## Firmware v2 features

- **Thermostat** that always runs in its own high-priority task on CPU core 1: SHT40 reading, hysteresis
  control, a flow setpoint computed from the room-temperature error (proportional plus integral), and
  OpenTherm master (Manchester PHY on GPIO29/30) with a simulated boiler. Failsafe: no sensor reading
  for 30 s turns heating off; frost protection at 7 °C.
- **Boot hardware check** of memory, touch, sensor, audio, Wi-Fi chip, camera and the OpenTherm line.
- **UI**: status bar (clock, heating, warnings, HA and Wi-Fi state), app launcher, thermostat pages
  (main, 24 h chart, placeholders for sensors/schedule/settings), Home Assistant dashboard with pages and
  an editor, calculator, Blocks game.
- **Setup on the device**: Wi-Fi, Home Assistant (token scanned from HA's QR code with the built-in
  OV02C10 camera), clock (NTP or manual), web login, thermostat tuning, display power (dim, screen off,
  keep-on schedule, night brightness, wake on camera motion), diagnostics, ESP32-C6 firmware update.
- **Web page** (HTTP Basic auth): settings, a read-only thermostat view with history, firmware upload
  (OTA with rollback if the boot check fails).

## Hardware

| Part | Connection |
|---|---|
| SHT40 room sensor | I2C 0x44 on CN3 (GPIO7 SDA / GPIO8 SCL, shared with touch, codec and camera) |
| OpenTherm shield | GPIO29 (shield → ESP), GPIO30 (ESP → shield), 3V3, GND |
| OV02C10 camera | MIPI-CSI, 1 lane; driver vendored from Guition's SDK (Apache-2.0) |
| ESP32-C6 | Wi-Fi coprocessor over SDIO (ESP-Hosted) |

## Build

```sh
. ~/esp/esp-idf/export.sh          # ESP-IDF 5.5
cd test-firmware-v2
idf.py build
idf.py -p /dev/cu.usbmodem* flash
```

Wi-Fi, Home Assistant and all other settings are entered on the device or on its web page and stored in
NVS; no credentials live in the source.
