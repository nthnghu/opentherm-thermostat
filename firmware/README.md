# Thermostat firmware (Guition JC4880P443C, ESP32-P4)

ESP-IDF 5.5 + LVGL 9 firmware: thermostat (always running, core 1), app launcher, Home Assistant cards,
calculator, Blocks, setup on the device and on a password-protected web page.

## Build and flash

```sh
. ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbmodem* flash        # NVS (Wi-Fi, HA, settings) is kept
```

Later updates can go over Wi-Fi: web page → Firmware → upload `build/thermostat.bin`.
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

## OpenTherm bus (Phase 1, version 0.2)

The design is in `docs/research/10-recommendation-phase1.md`.

| File | Role |
|---|---|
| `opentherm.c` | PHY: Manchester TX/RX through the DIYLESS shield, reply latency |
| `ot_bus.c` | Bus task (core 1, priority 21). It is the only user of the PHY and runs a 1 s cycle that never stops. It also keeps the frame capture: 32768 frames in PSRAM, about 3 h |
| `otb_core.c` | Master logic, pure C. See the cycle below |
| `ot_sim.c` + `plant_sim.c` | Simulated boiler, pure C. See below |
| `ot_frame.c` | Frame helpers and Data-IDs |

**One bus cycle (`otb_core.c`):**
- ID 0 status, retried once;
- ID 1 setpoint;
- one scheduled slot: live values, then the configuration record, the counters and the ID 57 mirror.

**Rules in `otb_core.c`:**
- Support map: 3 negative answers mark an ID unsupported. It is probed again hourly and after a reconnect.
- The link counts as lost after 5 failed cycles in a row.
- Immergas flags, all on by default: `ca` (ID 0 low byte 0xCA), `mirror57`, `explicitoff` (CH off = CH bit cleared + ID 1 = 10 °C).

**Simulated boiler (`ot_sim.c` + `plant_sim.c`):** an Immergas-like slave on a house, radiator and tank model. Its quirks can be switched on and off:
- writes ignored without 0xCA;
- heats to ID 57;
- ignores the CH bit;
- E31 after 60 s of silence, with CH locked until a power cycle;
- missed replies;
- faults.

Serial commands (USB):
- `OTSTAT`: link, boiler values, statistics, supported and unsupported IDs.
- `SIM show|reset|powercycle`, `SIM out <°C>`, `SIM room <°C>`, `SIM miss <%>`, `SIM silence 0|1`, `SIM fault <code>`, `SIM pressure <bar>`, `SIM leak <bar/day>`, `SIM needca|heats57|ignoreoff|dhwwrite 0|1`: drive the simulator.
- `SIM room 1|0`: the simulated house temperature replaces the SHT40 reading (bench closed loop, saved).
- `SIM ca|mirror57|explicitoff 0|1`: the bus flags (RAM only for now).

Web (login required):
- `/api/ot`: bus state and support map (JSON).
- `/api/otlog.csv[?n=N]`: the frame capture. Decode frames with `docs/research/interactive/opentherm-frame.html`.

## Control law (Phase 1a, `control.c`)

The control law is pure C and host-tested.

- **Strategy 1, PI load compensation** (default):
  - flow = 35 °C + Kp·e + I, updated every 60 s and slew-limited;
  - the integral freezes during DHW, duty pauses and saturation, and while the boiler is unreachable.
- **Low-load manager:**
  - below the minimum-fire flow it plans long burns (at least 10 min) with at most 3 CH starts per hour;
  - during a burn at minimum fire it holds the setpoint at flow − 3 °C.
- **Strategy 0, on/off baseline:** a fixed flow with hysteresis. Use it for the 2-week baseline.
- **Safety:**
  - room sensor lost, with three modes: `0` feature off, `1` heating off, `2` fixed flow with a duty limit (default 42 °C, 40 %);
  - frost floor in any mode (6 °C);
  - underfloor floor (mixer + 5 K), off by default.

Every feature has its own setting, saved in NVS (`thermo/ctrl`, versioned):
- `CFG` lists the settings with help text;
- `CFG <name> <value>` sets one (range-checked, saved after 5 s);
- `CFG defaults` restores the factory values;
- `/api/config` returns them as JSON.

On the screen, the status line shows the controller state: min-fire hold, long burn, pausing, frost, or sensor fallback.

Bench helpers:
- `MODE heat|off` and `SP <°C>`;
- `STAT` prints a `CTRL` line: state, setpoint, requested flow, P, I, duty, starts in the last hour.

## Schedule and hot water (Phase 1, `schedule.c`)

`schedule.c` is pure C and host-tested. It sits above the control law: it decides the room target, whether heating may run, and the hot water request. With every switch off it behaves like 0.2: constant setpoint, DHW always enabled, ID 56 never written.

| Feature | Switch | What it does |
|---|---|---|
| Weekly program | `prog_on` (off by default) | Comfort = your setpoint, Eco = Comfort − `eco_setback_k`. Up to 6 switch points a day. Without a valid clock: constant Comfort |
| Boost | `boost_on` | Comfort + `boost_k` for `boost_min`. Wins over Eco, away and summer |
| Away / holiday | `away_on` | `away_c` until a date or until cancelled (kept across restarts). Optionally hot water off (`away_dhw_off`) |
| Summer | `summer_on` (off by default) | Heating off while the 24 h outdoor mean is ≥ `summer_c`, back on below `summer_c − summer_hyst_k`. Needs an outdoor temperature (boiler ID 27 now, HA in Phase 1b). The frost floor still works |
| Hot water mode | `dhw_mode` | 0 = always enabled, 1 = up to 3 daily windows (`dhw_winN_start/end`), 2 = off. Without a clock the windows mode enables DHW |
| Tank guard | `guard_on` | Enables DHW outside the windows while the tank (ID 26) is below `guard_c`, until `guard_c + guard_hyst_k`. Also works while away |
| Piggy-back | `piggy_on` (off by default) | Enables DHW for `piggy_min` right after a heating burn (windows mode) |
| Hot water boost | `dhw_boost_min` (0 = off) | Enables DHW now, for that long |
| Tank setpoint | `dhw_set_on` (off by default) | Writes `dhw_set_c` to ID 56 |
| Pasteurisation | `pasteur_on` (off by default) | Weekly (`pasteur_day`, `pasteur_min`): ID 56 = `pasteur_c` until the tank holds `pasteur_c − 2` for `pasteur_hold_min` (gives up after `pasteur_timeout_min`), then the old setpoint is written back and ID 56 is left to the panel again |

**ID 56 check (`otb_core.c`):** every ID 56 write is read back in the next cycle. The result is `ok`, `ignored` (ACKed, but the boiler kept its own value, as an Immergas with P7 = 2 is expected to do) or `rejected`. Pasteurisation stops at once with `id56-ignored` when the boiler ignores the write; then it has to be done with the boiler's own routine. With the simulator: `SIM dhwwrite 0|1`.

Serial commands:
- `CFG` lists and sets these settings too (same names);
- `PROG` shows the week, `PROG <day 0..6> 06:00=c 08:00=e 17:00=c 22:30=e` sets one day (0 = Monday);
- `BOOST on|off`, `DHWBOOST on|off`, `AWAY on|off|<hours>`, `PASTEUR on|off`;
- `STAT` prints a `SCHED` line: source, target, next change, boost, away, summer, outdoor mean, DHW reason, ID 56 check, tank, last pasteurisation.

Web: `/api/status` has `schedule` and `dhw` objects; `/api/config` also returns these settings and the week as `prog`.

The screen's status line shows the plan, for example "Eco 19.0 °C until 05:30", "Boost 22.0 °C, 45 min left", "Away 15 °C until 11.20 18:00" or "Summer: heating off".

### Host tests

The pure-C parts build and run on the Mac, without ESP-IDF:

```sh
make -C firmware/tests/host
```
