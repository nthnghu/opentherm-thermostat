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

## Data log, KPIs, alerts and MQTT (Phase 1, milestone 4)

**Data log (`datalog.c`, pure C, host-tested):**
- one record per minute: room, target, flow/return/tank/pressure, modulation, flame and DHW seconds, starts, estimated gas input, controller state, flags, RSSI, OT errors;
- kept for 7 days in PSRAM. It is RAM only, so a restart clears it; the long history lives in VictoriaMetrics through MQTT;
- download from the web page: `/api/log.csv[?h=N]`.

**KPIs (last hour and last 24 h, `/api/kpi`, serial `STAT` → `KPI` line):**
- CH starts per hour, median CH burn, % burns shorter than 10 min;
- % CH burner time with the return below 55 °C, mean modulation, % time at minimum;
- comfort degree-hours (below target − 0.5 K while heating may run);
- gas input estimate.

The gas estimate assumes ID 17 is relative between the minimum and maximum input (ID 15), with the Tera 24 values as fallback. Check it against the gas meter.

**Alerts (`alerts.c`, pure C, host-tested):** each has its own switch and thresholds in `CFG`.

| Alert | Fires when |
|---|---|
| boiler fault | fault bit set, with the ID 5 code |
| low pressure | below 1.0 bar |
| pressure falling | dropped 0.3 bar over 24 h |
| link lost | after 60 s |
| room sensor lost | after 60 s |
| short cycling | ≥ 4 starts in the last hour with a median burn under 3 min |
| room not reaching target | 1 K below for 3 h |
| pasteurisation failed | the last run did not finish |
| no clock | after 10 min |

An alert clears 60 s after its condition is gone. The most important one is shown in the screen's status line and sent to HA.

**MQTT (`mqtt_ha.c`):** set it up on the web page under MQTT: broker `mqtt://host:1883`, user, password, location, device. The topics follow the monitoring stack's convention:
- `sensors/<location>/<device>`: state JSON every 10 s. Telegraf stores its numeric fields as `iot_<field>{location,device}`;
- `sensors/<location>/<device>/status/availability`: `online`/`offline` (last will);
- `sensors/<location>/<device>/cmd/{setpoint,mode,preset,dhw_boost}`: commands from HA. They are range-checked and act like the device buttons;
- `homeassistant/.../config`: discovery. It is sent after every connect and when HA restarts.

In HA the device "Thermostat" appears with:
- a climate entity: setpoint, Heat/Off, presets boost/away;
- the sensors and KPIs;
- one problem sensor per alert;
- a "Hot water boost" button.

The thermostat never depends on MQTT. Serial helpers:
- `MQTT` shows the settings (never the password), the connection state and NVS usage;
- `MQTT on|off` switches the client;
- `MQTT dump` prints the state JSON and a discovery payload.

## Service mode and commissioning (Phase 1, milestone 5, `service.c`)

`service.c` is pure C and host-tested against the simulator with each Immergas quirk switched on and off. The routines cover the ch. 10 test table, "Verify on the real boiler":

| Routine | Tests | What it does | Burns gas | Apply sets |
|---|---|---|---|---|
| `identity` | 6, 16 | Reads 3, 125, 127, 15, 6, 48, 49, 93–95 and probes 18, 19, 26–28, 33–36, 115–123 | no | – |
| `status_ca` | 4 | ID 57 write + read-back with ID 0 low byte 0x00, then 0xCA | no | `ca` |
| `writes` | 8 | ID 56 and 57: write a few degrees off, read back, restore | no | – |
| `setpoint` | 7 | ID 57 = 60, ID 1 = 30/45/40 (`svc_step_min` each), compares the flow | ~30 min | `mirror57` |
| `ch_off` | 9, 15 | During a burn: CH bit cleared with ID 1 kept, then with ID 1 = 10; pump post-run | ~10 min | `explicitoff` |
| `id14` | 10 | During a burn: ID 14 = 30/100/0 %, watches ID 17 | ~12 min | – |
| `min_fire` | 12 | Holds TSet = flow − 3 at minimum modulation until the flow is stable (OPV) | up to 40 min | `flow_min_c` |
| `cutout` | 13, 14 | Lowers TSet until the cut-out, then times re-ignition (anti-cycle) and the power ramp | ~15 min | – |
| `comm_loss` | 18, 20 | Silent for `svc_silence_s`, then reads ID 5 (E31?) and checks whether heating works again | – | – |

`comm_loss` needs explicit confirmation: on the Tera, E31 is expected to lock heating until the boiler is power-cycled.

**How a routine runs:**
- It replaces the normal request (CH/DHW/flow setpoint, bus flags) step by step and restores the bus flags afterwards.
- It aborts on any of these:
  - the boiler stops answering or reports a fault for 10 s (except `comm_loss`);
  - the room drops below the frost limit;
  - its time limit;
  - Cancel.
- Results go into the commissioning record, saved in NVS. Each result carries the time and a "(sim)" mark when it was measured on the simulated boiler.
- Service mode ends by itself after `svc_idle_min` (30 min) without activity.
- One-off reads of any ID, and writes to IDs 1, 14, 56 and 57 only, are possible while service mode is on and no routine runs.

The bus flags (`ca`, `mirror57`, `explicitoff`) are now saved in NVS: by Apply and by `SIM ca|mirror57|explicitoff`. A routine's temporary changes stay in RAM.

**Where to use it:**
- Serial:
  - `SVC` shows the state and the record;
  - `SVC on|off`, `SVC run <routine> [confirm]`, `SVC cancel`, `SVC apply <routine>`;
  - `OT read <id>`, `OT write <id> <°C|%|0xRAW>`;
  - `CFG svc_*` for the settings.
- Web: the Service section on the main page, and `/api/service` (JSON).
- Screen: the status line shows the running routine and its step.
- MQTT: `service` and `service_routine` fields.

## User interface (Phase 1, milestone 6)

**Thermostat app, swipe pages:**
1. Room, setpoint, mode. The status line shows the plan, alerts and service; tap it for the boiler card.
2. 24 h chart.
3. **Boiler card:** link (simulated/real), burner, flow/return, modulation, setpoint, pressure, tank, controller state, starts, burns, efficiency and gas estimate (24 h), active alerts.
4. **Schedule:**
   - the week as Comfort (orange) / Eco (blue) bars; tap a day to edit its switch times (±15 min, Comfort/Eco, add/delete, copy to other days);
   - program on/off;
   - Boost and Away;
   - hot water mode (Always / Windows / Off), Hot water boost, tank and pasteurisation status.
5. **Settings:**
   - everyday shortcuts: weekly program, Eco temperature drop, hot water, summer mode, frost protection and limit, what to do if the room sensor fails, underfloor circuit. Each row shows a plain title, the value with its unit and a one-line description;
   - **All settings:** nine topics (program, summer, hot water, legionella, safety, heating system, fine tuning, alerts, service tests). Settings that belong to another are indented, and dimmed while that one is off;
   - the editor shows what the setting does, which setting it depends on, the range, the factory value, and Off/On, previous/next choice or −/+ buttons (long press repeats);
   - **Service:** asks for the service PIN (set on the web page; locks again after 10 min, 5 wrong tries = 5 min wait). Shows service mode on/off, the routines with their last results, Start/Cancel and Apply. `comm_loss` asks for confirmation.

The titles, explanations, units and choice names live in `cfgtext.c` (host-tested: every setting has one, no internal names, texts fit the screen). Internal names stay for the serial `CFG` command and JSON. The alert switch for a failed hot cycle is `alert_pasteur_on` (it used to clash with the schedule's `pasteur_on`).

**Wi-Fi chip watchdog:** the Wi-Fi radio is a separate ESP32-C6, asked over SDIO; when it hangs every request blocks for 5 s. Only the background task `wifi_mon` (board.c) asks it, every 5 s, and caches the signal strength, so the screen and MQTT never wait on it. When the C6 has not answered for `wifi_wd_min` minutes (web page, Wi-Fi section, or serial `WIFIWD <min>`; default 3, 0 = never) the device restarts, which resets the C6. Heating pauses for a few seconds. `STAT` prints a `WIFI` line (seconds since the chip last answered, restarts so far).

After the "Back to thermostat" time without a touch, the device returns to the thermostat's main page, also from pages 2-5 and from the editors.

All editors use `cfgall.c`, one index over the control, schedule, alert and service tables. The serial `CFG`, the web form and JSON import use the same index. `CFG defaults` keeps the weekly program.

**Web page:**
- "Thermostat settings": the same titles, explanations, topics and choice lists as the device, with range and factory value (the internal name is the tooltip);
- "Weekly program": one line per day, `06:00=c 08:00=e`;
- "Export / import": `/api/config?download=1` gives all settings, the program and the boiler flags as JSON; paste a file to restore it. Unknown keys are ignored and values are range-checked;
- the service PIN.

**Home Assistant, everyday controls:** switches "Weekly program" and "Summer mode (auto)", a select "Hot water mode", and a number "Eco setback".

Serial screens for screenshots: `GO page N`, `GO edit <name>`, `GO advanced`, `GO group <0..8>`, `GO service`, `GO day <0..6>`.

### Updates and diagnostics without USB

- **Update:** `firmware/tools/ota.sh` builds, uploads to `/ota`, waits for the restart and checks that the new build (ELF SHA) runs and was accepted. The web page (Firmware) does the same upload by hand.
- **Trial (`ota_check.c`):** a new firmware is kept only when the hardware check passes and, within 5 minutes, Wi-Fi is connected and the device's own web server answers (asked over loopback). Otherwise it rolls back to the previous firmware. A restart during the trial also rolls back.
- **Remote diagnostics** (web login): `POST /api/cmd` runs any serial command (`STAT`, `CFG`, `SVC`, `OTSTAT`, `SIM`, `GO` ...) and returns its output (`debug_exec`); `GET /api/log` returns the last 64 KB of the log (`logring.c`); `GET /api/screen.bmp` returns a screenshot; `GET /api/version` returns the version, build SHA and update state. The web page has a Diagnostics section with a console.
- Login for the scripts: `~/.config/thermostat.netrc` (`machine <ip> login <user> password <password>`, chmod 600).
- Motion detections are logged at debug level now, so they don't fill the log.

### Host tests

The pure-C parts build and run on the Mac, without ESP-IDF:

```sh
make -C firmware/tests/host
```
