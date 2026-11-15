# Thermostat firmware (Guition JC4880P443C, ESP32-P4)

ESP-IDF 5.5 + LVGL 9 firmware: thermostat (always running, core 1), app launcher, Home Assistant cards,
calculator, Blocks, setup on the device and on a password-protected web page.

## Build and flash

```sh
. ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbmodem* flash        # NVS (Wi-Fi, HA, settings) is kept
```

Later updates can go over Wi-Fi: web page → Firmware → upload `build/p4_test_v2.bin`.
A new image is kept only if the boot hardware check passes; otherwise the device rolls back.

## Hardware used

| Part | Bus / pins |
|---|---|
| SHT40 room sensor | I2C 0x44 on CN3 (GPIO7 SDA / GPIO8 SCL, shared with GT911 0x5D, ES8311 0x18, camera 0x36) |
| OpenTherm shield | GPIO29 in (shield → ESP), GPIO30 out |
| OV02C10 camera | MIPI-CSI 1 lane, 1288×728; driver vendored in `components/ov02c10` (Guition SDK, Apache-2.0) |
| ESP32-C6 Wi-Fi | ESP-Hosted over SDIO |

## Wi-Fi chip (C6) update

The C6 ships with ESP-Hosted 2.3.2; the host is 3.1.0 (works, but warns). To update:

```sh
cd ../c6-coprocessor && idf.py build            # ESP-Hosted 3.1.0 coprocessor for esp32c6
cd ../firmware
tools/flash_c6fw.sh ../c6-coprocessor/build/eh_cp_ota_coprocessor_ota.bin
```

Then on the device: Apps → Setup → Diagnostics → Update Wi-Fi chip.

## Development helpers (USB serial only)

- `GO launcher|thermo|ha|calc|blocks|setup|page N|panel N` switches screens (`tools/go.py PORT "panel 5"`).
- `SNAP` dumps the frame buffer as a PNG (`tools/snap.py PORT out.png`).
- `STAT` prints the thermostat state; `tools/serlog.py PORT SECONDS out.log` captures the log.

Run them with the ESP-IDF Python (it has pyserial).
