# Firmware review

Date: 2026-11-16 · Commit reviewed: `795f2fe` (firmware 0.3.1) · No code was changed.

## Scope and method

- **Read in full:** `thermostat.c` (task loop, load/save, setters), `opentherm.c`, `ot_bus.c`, `control.c` (sensor-loss and frost paths), `sht4x.c`, `ota_check.c`, `logring.c`, `timekeep.c`, `main.c`, the auth, console, OTA, sink and save handlers of `webui.c`, MQTT client setup and command handling in `mqtt_ha.c`, the HTTP client in `ha.c`, Wi-Fi monitor and reconnect in `board.c`, slot writing in `histstore.c`, password handling in `settings.c`.
- **Only searched, not read line by line:** the UI files (`ui*.c`, `app_*.c`, `quickpanel.c`), `camera.c`, `motion.c`, `hadash.c`, `service.c`, `schedule.c`, `datalog.c`, `c6update.c`, `selftest.c`. Findings there come from pattern searches and the tools below.
- **Not reviewed:** the vendored components and `managed_components/`, the C6 co-processor build.

**Status (same day, after the review):** H1–H5 are fixed in the commit that adds this file. H6 is open. The findings below describe the code as reviewed (`795f2fe`), so their line numbers refer to that commit.

| # | Fix | Checked |
|---|---|---|
| H1 | `CONFIG_ESP_TASK_WDT_PANIC=y`: a control task that hangs for 10 s restarts the device | On the device (`HANG 40`): restart about 11 s after the task stopped, web page back 5 s later, firmware still accepted, PI correction and burner-start count restored |
| H2 | The bus switches heating off when the controller has sent no command for 10 s (`otb_cmd_guard`) | Host test `test_cmd_guard`, and on the device (`HANG 25 feed`, burner on): flow request 55 → 10 °C at 10–11 s, burner off, back to 55 °C when the task continued |
| H3 | With no saved choice the thermostat looks for a real boiler for 120 s and only then falls back to the simulator, without saving that | Read-through only; the bench device has a saved choice, so this path did not run |
| H4 | Bus and thermostat start before the display; a failed panel leaves a headless UI; timer failure is no longer fatal | Normal boot on the device (thermostat starts at 1.6 s). The no-display path was not exercised |
| H5 | POSTs with an `Origin` or `Referer` from another site are refused (403) | On the device: foreign Origin, `null`, look-alike host and foreign Referer refused; same-origin and curl accepted |

**Medium findings, fixed later the same day** (M3 is intentional: later only OTA updates will be possible, so a firmware that cannot reach Wi-Fi must roll back; M5, M11, M12 are open):

| # | Fix | Checked on the device |
|---|---|---|
| M1 | The Wi-Fi watchdog first resets the Wi-Fi chip alone (driver and link down, link up, reconnect); the device restarts only if the chip is still silent 90 s later | Three chip resets in a row (`C6RESET`): 2.4 s each, no restart, no memory lost. A real hang cannot be provoked. A chip that is completely dead still restarts the device: esp_hosted itself aborts on "TRANSPORT_FAILURE" |
| M2 | `save()` checks every write, logs failures and retries after a minute; the away end time is written before the away flag; the setpoint is range-checked on load; an NVS erase or init error is logged | Normal saves only; no write failure was provoked |
| M4 | An upload with no data for 30 s is dropped | Stalled `/ota` upload: dropped after 30 s, web page answering again, current firmware kept |
| M6 | New Wi-Fi credentials are on trial for 60 s; without a connection the previous ones are restored | Bogus network saved through the web page: back on the old network after 63 s |
| M7 | Soft reset of the sensor after 5 failed reads, I2C bus reset after 15; bit-identical readings for 5 minutes count as a failure | Not exercised (needs a faulty sensor) |
| M8 | QR token scanner removed (32 KB task stack, 225 KB preview buffer, quirc); allocations above 64 bytes go to PSRAM | Free DMA RAM 98 KB, low point 89 KB (63 / 52 KB before, 26 / 17 KB in the morning) |
| M9 | Stack overflow check, light heap poisoning, core dump to a new `coredump` flash partition, summary in the log at boot, download at `/api/coredump`, console `COREDUMP [clear]` | Watchdog crash provoked: dump stored, reported at boot, downloaded and decoded with `idf.py coredump-info` |
| M10 | After 5 wrong logins every further one blocks logins for 10 s, doubling up to 5 minutes | 5 × 401, then 429 for 10 s, also for the correct password. The hash is still one round of SHA-256 |
| M13 | `esp_hosted` 3.1.0 and `esp_wifi_remote` 1.6.5 pinned exactly; `dependencies.lock` is now tracked in git | Build |

**H6, partly done (firmware 0.4.0):**

| Part | State |
|---|---|
| HTTPS on the device | Done. The web server runs on port 443 with a key and self-signed certificate made on the device at first start (`tlscert.c`, kept in NVS, nothing secret in the image or the repo). Port 80 redirects page views and refuses everything else. `tools/ota.sh` pins the device's public key (`~/.config/thermostat.pin`, trusted on first use; console `TLS` shows it, `TLS new` replaces the certificate). Checked on the device: pin matches, wrong pin refused, POST over HTTP gets 403, handshake 0.17–0.22 s, main page 1.1 s, upload about 800 KB/s, free DMA RAM 88 KB |
| Signed firmware | Skipped for now by decision. A signing key was generated at `~/.config/thermostat-signing-key.pem` and is unused; the firmware accepts unsigned images as before |
| Home Assistant over `https://` | Not a firmware change; depends on the HA setup |

Severity: **High** = can leave the house unheated, or gives control of the device to someone else. **Medium** = degrades reliability or security in a realistic case. **Low** = hardening or cleanup.

Items marked *(verify)* are conclusions from reading code that I did not reproduce on the device.

## Summary

| # | Severity | Area | Finding |
|---|---|---|---|
| H1 | High | Reliability | Task watchdog only logs; a hung control task is never recovered |
| H2 | High | Reliability | Bus task repeats the last command forever if the control task stops |
| H3 | High | Reliability | Defaults to the simulated boiler when NVS is empty or unreadable |
| H4 | High | Reliability | Display init failure aborts the boot before heating control starts |
| H5 | High | Security | No CSRF protection: any web page can install firmware or run commands through a logged-in browser |
| H6 | High | Security | Web login, tokens and firmware travel over plain HTTP; firmware is unsigned |
| M1 | Medium | Reliability | Wi-Fi chip hang restarts the whole thermostat, including heating control |
| M2 | Medium | Reliability | NVS "no free pages" silently erases all settings; other NVS errors are ignored |
| M3 | Medium | Reliability | OTA trial rolls back a good firmware when Wi-Fi is down, and never checks the control loop |
| M4 | Medium | Reliability | OTA and sink uploads retry socket timeouts forever and block the web server |
| M5 | Medium | Reliability | OpenTherm ISRs call functions that are not placed in IRAM; only history writes are fenced off |
| M6 | Medium | Reliability | New Wi-Fi credentials are saved before they are proven to work |
| M7 | Medium | Reliability | Sensor has no recovery (soft reset, bus reset) and no stuck-value detection |
| M8 | Medium | Resources | Internal DMA RAM is still the tightest resource (63 KB free) |
| M9 | Medium | Resources | No stack overflow detection, heap poisoning or core dump |
| M10 | Medium | Security | No login rate limit; password hash is one round of salted SHA-256 |
| M11 | Medium | Security | MQTT is plain TCP, and MQTT commands change setpoint and mode |
| M12 | Medium | Security | Secrets are stored unencrypted in flash (no flash or NVS encryption) |
| M13 | Medium | Security | Dependencies pinned with `*`; no secure boot |
| L1–L12 | Low | Mixed | See the Low section |

---

## High

### H1. Task watchdog only logs; a hung control task is never recovered
- `sdkconfig`: `# CONFIG_ESP_TASK_WDT_PANIC is not set`; `sdkconfig.defaults:55-56` only sets the timeout.
- `thermostat.c:382,387` and `ot_bus.c:144,147` subscribe the two control tasks to the watchdog.

With panic off, a task that stops feeding the watchdog only produces a log line every 10 s. If `thermo_task` deadlocks (for example on `s_lock`, the I2C mutex taken with `portMAX_DELAY` in `sht4x.c:67`, or a stuck flash write), the device keeps its screen and web page alive but stops controlling.

**Fix:** `CONFIG_ESP_TASK_WDT_PANIC=y` in `sdkconfig.defaults`. Combined with H2, a hang then leads to a restart within 10 s.

### H2. Bus task repeats the last command forever if the control task stops
- `ot_bus.c:150` copies `s_cmd` into the bus core every cycle; nothing checks how old it is.
- `thermostat.c:644` (`otbus_set_cmd`) is the only writer.

If the control task hangs while "heating on, flow 55 °C" is the last command, the bus task keeps sending it once a second. The boiler sees a healthy link, so it never raises E31 and keeps heating with no room feedback. This is the one path I found where the failure direction is "too hot", not "cold".

**Fix:** timestamp each `otbus_set_cmd`; when the command is older than about 10 s, the bus task sends "CH off" (or the frost setting). H1 then restarts the device.

### H3. Defaults to the simulated boiler when NVS is empty or unreadable
- `thermostat.c:214` `s_st.boiler_sim = true;` and `ot_bus.c:31` `static volatile bool s_sim = true;`.

On the bench this is right. On the wall it means: if NVS is erased (see M2), corrupted, or the "thermo" namespace fails to open, the thermostat silently talks to the built-in simulator and the screen shows plausible temperatures. The real boiler gets no frames, raises E31, and per `docs/research/03-victrix-tera-24-plus.md` may stay off until it is power-cycled.

**Fix:** a build-time or one-time provisioning choice for the default, and a visible warning (alert plus a banner) whenever the simulated boiler is active outside service mode.

### H4. Display init failure aborts the boot before heating control starts
- `main.c:33` `ESP_ERROR_CHECK(board_display_init());` runs before `otbus_start()` and `thermo_start()` at `main.c:41-43`.
- `opentherm.c:111` `ESP_ERROR_CHECK(gptimer_new_timer(...))` has the same effect for a timer failure.

A broken display cable, a panel fault or a failed PSRAM frame-buffer allocation leads to an abort, a restart, and the same abort again. Heating never starts. The comment at `main.c:40` says the thermostat should start before anything slow; the display is the exception.

**Fix:** start the bus and thermostat tasks first, and treat a display failure as a logged, degraded mode (web page and MQTT still work).

### H5. No CSRF protection on the web interface
- `webui.c:1048` (`/save/*`), `webui.c:1358` (`/api/cmd`), `webui.c:1498` (`/ota`) accept any POST that carries valid Basic credentials. There is no token, no `Origin` or `Referer` check, and no required custom header.

A browser that has logged in once re-sends the Basic credentials automatically. Any web page open in that browser can then send a cross-site POST to `http://10.10.102.121/ota` with a firmware image as the body, or to `/api/cmd` with `RESTART`, `CFG ...` or `SVC ...`. These are "simple" requests, so the browser sends them without a preflight. *(verify: I did not build a proof of concept.)*

**Fix:** reject state-changing requests unless `Origin`/`Referer` matches the device, or require a custom header (`X-Requested-With`) plus a per-session token in the forms.

### H6. Login, tokens and firmware over plain HTTP; firmware is unsigned
- `webui.c:182-190`: HTTP Basic, so the password crosses the network base64-encoded on every request.
- `webui.c:1511-1543`: `esp_ota_begin/write/end` checks only the image format and its own SHA; there is no signature and no version check.
- `sdkconfig`: `# CONFIG_SECURE_BOOT is not set`.
- The Home Assistant long-lived token is sent as a Bearer header to an `http://` URL (`ha.c:36-38`, device setting `http://10.10.200.30:8123`).

Anyone who can observe traffic on the VLAN gets the web password and the HA token, and the web password is enough to install arbitrary firmware.

**Fix, in order of value:** signed app images (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` gives OTA signature checks without burning eFuses); HTTPS for the device web server; `https://` to Home Assistant.

---

## Medium

### M1. A Wi-Fi chip hang restarts the whole thermostat
- `board.c:655-659`: after `wifi_wd_min` minutes of C6 silence, `esp_restart()`.

The device has restarted 27 times for this reason. Each restart stops OpenTherm frames for several seconds, resets the low-load duty state, and (before `33257c6`) reset the starts-per-hour count. Heating control does not need Wi-Fi, so its availability should not depend on the Wi-Fi chip.

**Fix:** reset only the C6 (reset pin, then re-init esp_hosted) and keep the P4 running; fall back to a full restart only if that fails.

### M2. NVS erase and ignored NVS errors
- `main.c:26-30`: on `ESP_ERR_NVS_NO_FREE_PAGES` or `NEW_VERSION_FOUND` the whole NVS is erased, with no log line and no alert. That removes Wi-Fi, web login, schedule, control settings and the boiler choice (see H3). Any other error from `nvs_flash_init()` is ignored.
- `partitions.csv:2`: NVS is 24 KB (6 pages), shared by settings, four config blobs, service records and the Wi-Fi hang log.
- `thermostat.c:244-262` (`save()`) writes seven keys without checking results; a power cut between them can leave `away` and `awayuntil` out of step.

**Fix:** log and raise an alert when NVS was erased; check and report NVS write errors in `save()`; consider a larger NVS partition at the next USB flash.

### M3. OTA trial judges the wrong things
- `ota_check.c:45-52`: a new image is kept once Wi-Fi is up and the web server answers.
- `ota_check.c:54-58`: no Wi-Fi for 5 minutes means rollback, even when the firmware is fine and the router is down.
- Nothing checks that `thermo_task` and `bus_task` are looping, that the sensor reads, or that the OpenTherm link came back.

**Fix:** add "control loop count increased, sensor OK, boiler link state same as before the update" to the acceptance test; when Wi-Fi is missing but those pass, extend the trial instead of rolling back.

### M4. Uploads can block the web server forever
- `webui.c:1520-1522` (OTA) and `webui.c:1407-1409` (sink): `if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;` with no limit.

`esp_http_server` handles one request at a time. A client that opens an upload and stalls keeps the server task in this loop, so the web page, `/api/*` and OTA are unreachable until the socket dies.

**Fix:** give up after a number of consecutive timeouts (for example 30 s without data) and abort the OTA.

### M5. OpenTherm interrupt handlers are not fully cache-safe
- `opentherm.c:29-43` (`tx_alarm`) calls `gpio_set_level` and `gptimer_stop`; `opentherm.c:47-93` (`rx_edge`) calls `gpio_get_level`.
- `sdkconfig`: `# CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM is not set`, `# CONFIG_GPIO_CTRL_FUNC_IN_IRAM is not set`, `# CONFIG_GPTIMER_ISR_CACHE_SAFE is not set`; the GPIO ISR service is installed without `ESP_INTR_FLAG_IRAM` (`opentherm.c:104`).
- `ot_bus.c:76-88` and `histstore.c:127-136` keep history writes away from frames, but NVS writes (`thermostat.c:244`, `settings.c:122`) and OTA writes (`webui.c:1527`) are not fenced.

Since `CONFIG_SPIRAM_XIP_FROM_PSRAM=y` the code runs from PSRAM, so a flash write probably no longer stops these handlers. I did not confirm that on the device. If the cache is still paused, a frame sent during an OTA or NVS write is corrupted and retried. *(verify: count `timeouts + bad` on the real boiler during an OTA.)*

### M6. New Wi-Fi credentials are saved before they work
- `board.c:476-481`: `settings_save()` happens before `esp_wifi_connect()`.

A typo in the web form leaves the device off the network with the old, working credentials gone. Recovery needs the touch screen.

**Fix:** keep the previous credentials and return to them after N failed attempts.

### M7. Room sensor: no recovery and no stuck-value check
- `sht4x.c:42-60`: a failed read returns the error; there is no soft reset (command 0x94) and no I2C bus recovery.
- `thermostat.c:392-394`: `sht4x_init` is retried only when adding the device failed, not after read errors.
- `thermostat.c:407`: the only plausibility check is −20 … 60 °C. NaN is rejected by this comparison, which is good, but a sensor that returns the same valid value forever is trusted.
- The I2C bus is shared with touch, audio codec and camera (`board_i2c_bus()`); a device holding SDA low takes the sensor down with it.

The fallback itself is sound: after `THERMO_SENSOR_STALE_S` the control goes to the configured loss mode (`control.c:315-338`) and an alert is raised.

### M8. Internal DMA RAM remains the tightest resource
- Measured today with the new `HEAP` command: 63 KB free, 52 KB low point under load (was 26 KB and 17 KB before `ae5a91f`).
- Still in internal RAM: about 60 KB of task stacks (`thermostat.c:716` 8 KB, `webui.c` httpd 10 KB, `app_setup.c:1247` 10 KB, `board.c:222` LVGL 8 KB, `mqtt_ha.c:600,653` 2 × 6 KB, `debug.c:466` 6 KB, others 4 KB each).
- `app_setup.c:379`: the QR scanner takes a 32 KB internal stack while it runs. That is half of the free DMA RAM, at the moment the user is setting up the network.
- The esp_hosted fixed frame pool (`CONFIG_ESP_HOSTED_HOST_SDIO_USE_MEMPOOL`) is off; it needs 47 KB contiguous at start, which is now available.

**Suggestions:** enable the frame pool so Wi-Fi no longer competes with other allocations; give the QR task a PSRAM stack and do its one flash write from another task; log the 10-minute memory line for a week before deciding more.

### M9. No overflow or crash diagnostics
- `sdkconfig`: `CONFIG_COMPILER_STACK_CHECK_MODE_NONE=y`, `CONFIG_HEAP_POISONING_DISABLED=y`, `CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=y`, `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y`.

A stack overflow or heap corruption shows up as an unexplained restart. The restart-surviving log ring (`logring.c`) helps, but the panic backtrace is printed by the panic handler, not through `esp_log`, so it is not in the ring. *(verify)*

**Fix:** `CONFIG_COMPILER_STACK_CHECK_MODE_NORM`, `CONFIG_HEAP_POISONING_LIGHT`, and a core dump to a small flash partition. Add `uxTaskGetStackHighWaterMark` to the `TASKS` command to size stacks from data.

### M10. Login hardening
- `webui.c:193`: failed logins are logged, but there is no delay, counter or lockout, so the password can be guessed at network speed.
- `settings.c:193-203` (`hash_pw`): the stored hash is one round of SHA-256 over salt and password. The comparison is constant-time (`settings.c:215`, `settings_check_web_password`), and the salt is random, which is good.
- `ui_pages.c:1006`: the service PIN is at most 7 digits with no attempt limit that I could find.

### M11. MQTT is unencrypted and can command the thermostat
- `mqtt_ha.c:588-601`: no TLS settings, so only `mqtt://` works; user name and password go in clear text.
- `mqtt_ha.c:453-475`: anyone who can publish to the command topics sets the setpoint, mode, boost and away. The setpoint is range-checked (`mqtt_ha.c:459`), which is good.

Protection is the broker's ACL only. Confirm that the thermostat's command topics are writable only by Home Assistant.

### M12. Secrets at rest
- `sdkconfig`: `# CONFIG_SECURE_FLASH_ENC_ENABLED is not set`, `# CONFIG_NVS_ENCRYPTION is not set`.
- `settings.c:133,135,165`: Wi-Fi password, HA token and MQTT password are stored as plain strings.

Someone with physical access and a USB cable can read them with `esptool read_flash`. The USB console (`debug.c:420-436`) also accepts every command without a login.

### M13. Supply chain
- `main/idf_component.yml`: `espressif/esp_wifi_remote` and `espressif/esp_hosted` use version `*`; others use caret ranges.
- `dependencies.lock` pins the actual versions (esp_hosted 3.1.0), so builds are reproducible as long as the lock file is kept. A `idf.py update-dependencies` would jump to whatever is newest, including the SDIO behaviour that caused the C6 hangs.

**Fix:** pin exact versions for esp_hosted and esp_wifi_remote. I did not check the pinned versions against published advisories.

---

## Low

| # | Where | Finding |
|---|---|---|
| L1 | `thermostat.c:220-222` | Setpoint, offset and away time are loaded from NVS without range checks. Setters clamp (`thermostat.c:728-729`), the load path does not. |
| L2 | `thermostat.c:265-281` | `s_dirty_since_us` (64-bit) is written from other tasks without the lock. On the 32-bit P4 a torn read can delay or skip one save. |
| L3 | `thermostat.c:399` | `s_st.sim_room` and `s_st.boiler_sim` are read outside the lock. |
| L4 | `thermostat.c:562-565` | The two KPI passes over up to 1440 records run once a minute while holding `s_lock`, which the UI and web handlers wait on. |
| L5 | `settings.c:105-108`, `timekeep.c:34` | `settings_get()` hands out a pointer to the live struct; `tz` is read without the lock while a save may be writing it. |
| L6 | `settings.c:217`, `timekeep.c:41` | A 2.5 KB `settings_t` on the stack, once per web request in the first case. Fine on the 10 KB httpd stack, risky if called from a small one. |
| L7 | `debug.c:1` | The header comment says the console is "not reachable over the network", but `/api/cmd` (`webui.c:1358`) exposes it. |
| L8 | `ui_pages.c:1016-1019` | `dots[16]` holds exactly 7 PIN digits × 2 + NUL. Correct today; it overflows if `s_pin` grows. |
| L9 | `timekeep.c:26-30,98` | SNTP steps the clock. A backwards step moves epoch-based ends (hand override, away) and can put history records out of order. No RTC battery, so after a power cut the schedule runs as Comfort until NTP answers (alert raised, by design). |
| L10 | `opentherm.c:47-93` | No guard against an edge storm on a noisy or floating input; the interrupt watchdog (800 ms) is the only limit. |
| L11 | `mqtt_ha.c:455`, `logring.c` | Command payloads and the HA URL go to the log, which `/api/log` serves. No passwords or tokens are logged (`settings.c:101`, `ha.c:61` log only "set" or the length). |
| L12 | `datalog.c:374,382,393` | Return value of `datalog_get()` ignored; see the analyzer result below. |

**Flash wear (checked, no action needed):** room and control settings are debounced by 5 s (`thermostat.c:29`); the history rings erase each sector about every 26 h (`histstore.c:129-131`), far below the flash rating; MQTT state is not written to flash. The Wi-Fi hang log writes NVS once per watchdog restart (`board.c:657`).

**Reconnect logic (checked, looks right):** Wi-Fi reconnects from a background task every 3 s (`board.c:394`, `board.c:589`), MQTT reconnects every 10 s (`mqtt_ha.c:595`), and neither blocks the control tasks on core 1.

---

## Tool results

| Tool | Result |
|---|---|
| cppcheck, clang-tidy, scan-build, flawfinder | Not installed on this machine; not run. |
| `clang --analyze` (Apple clang 21) on the 11 host-testable files | 3 warnings, all `datalog.c:374,382,393`: "assigned value is uninitialized". Cause: `datalog_get()`'s return value is ignored. Not reachable today because the index is always in range, but the check is cheap to add. |
| Host tests with AddressSanitizer + UBSan | All 2 992 checks pass; no sanitizer reports. |
| Host build `-Wall -Wextra -Werror` | Clean (this is the Makefile default). |
| Extra warnings (`-Wshadow -Wconversion -Wdouble-promotion -Wfloat-equal`) | 120 warnings: 88 double-promotion, 14 int-to-float, 6 float-equal (`cfgtable.c:25,34`, `schedule.c:197`, `service.c:1044,1046`), 6 float-to-bool (`plant_sim.c:120,130`, `service.c:1037-1052`), 4 enum-to-uint8, 2 shadow (`schedule.c:196,239`). None is a bug I could confirm; the float-to-bool and float-equal ones deserve a look. |
| Coverage of host tests (llvm-cov) | 90 % of lines, 77 % of branches in the 11 host-testable files; table below. |
| ESP-IDF build with extra warnings | Not run. The normal IDF build of today's commits completed without errors; I did not collect its warning list. |

| File | Lines | Branches |
|---|---|---|
| `ot_frame.c` | 100 % | 100 % |
| `ot_sim.c` | 98 % | 91 % |
| `plant_sim.c` | 96 % | 91 % |
| `cfgtable.c` | 95 % | 74 % |
| `datalog.c` | 94 % | 71 % |
| `schedule.c` | 92 % | 80 % |
| `alerts.c` | 90 % | 75 % |
| `otb_core.c` | 88 % | 86 % |
| `control.c` | 87 % | 80 % |
| `service.c` | 86 % | 73 % |
| `cfgtext.c` | 86 % | 55 % |

---

## Untested paths and proposed tests

### No tests at all (need ESP-IDF, so not in the host suite)
`thermostat.c`, `ot_bus.c`, `opentherm.c`, `sht4x.c`, `histstore.c`, `settings.c`, `timekeep.c`, `logring.c`, `ota_check.c`, `board.c`, `webui.c`, `mqtt_ha.c`, `ha.c`, `c6update.c`, and all UI files. That is about 12 000 of 18 500 lines, including every line that touches flash, the network or an interrupt.

The largest gain would come from moving three pieces of pure logic out of them into host-testable files:

| Extract | From | Why |
|---|---|---|
| Sensor filter and validity (range check, smoothing, stale timer, display dead band) | `thermostat.c:403-437` | The NaN / out-of-range / stale logic is safety-relevant and untested. |
| Manchester decoder state machine | `opentherm.c:47-93` | Feed it edge timestamps; no hardware needed. |
| History ring scan, CRC and restore order | `histstore.c:75-115` | Power-loss behaviour can be tested by truncating a fake partition image. |

### Untested branches in tested files
| Where | What is not exercised |
|---|---|
| `control.c:115` | `flow_min_c > flow_max_c` in sanitize |
| `control.c:155-158` | `ctrl_set_config` at run time, including the integral clamp when the limit is lowered |
| `control.c:222-223` | Min-fire hold state |
| `control.c:231-233`, `246-248` | Low-load: off while replanning, and the branch that ends an idle wait |
| `schedule.c:415,420,461-472` | Pasteurisation: ID 56 ignored, restore pending, cancel |
| `otb_core.c:121,131-132` | Bad frame type in the reply |
| `otb_core.c:315,430` | "Data invalid" replies |
| `otb_core.c:425` | DHW setpoint rejected by the boiler |
| `datalog.c:206-207,252` | DHW starts and DHW burner seconds |
| `datalog.c:239-245` | Tank, outside and pressure averages |
| Functions never called | `ctrl_set_config`, `svc_set_config`, `svc_exit`, `datalog_burn_count`, `plant_min_fire_flow`, and the name/help lookups |

### Proposed test cases
| # | Case | Expected |
|---|---|---|
| T1 | Sensor returns NaN, −45 °C (all-zero word), 130 °C (all-ones word), then valid | Rejected readings count as errors; `sensor_ok` goes false after the stale time; the first valid reading restarts the filter without a jump |
| T2 | Sensor stuck at one valid value for 6 h while heating at full power | Today: accepted. Decide on a "no change while heating" alert, then test it |
| T3 | Sensor lost in each `loss_mode` (disabled, fixed duty, off), in Heat and Off | Output as configured; frost floor not applied without a sensor (confirm that this is intended, `control.c:310`) |
| T4 | Setpoint from MQTT: `4.9`, `30.1`, `abc`, empty, `21.5xyz`, `nan`, `1e9` | Out-of-range and non-numeric ignored; `21.5xyz` currently accepted as 21.5 (`mqtt_ha.c:458-459`): decide whether that is wanted |
| T5 | NVS holds setpoint 500 (50.0 °C) or −100 | Clamped on load (fails today, see L1) |
| T6 | Config blob with a wrong version, wrong length, or all 0xFF | Defaults used and a log line; no partial struct |
| T7 | Clock not set at boot, then NTP sets it mid-override | Schedule runs Comfort with the clock alert, then follows the program; the override end is computed once |
| T8 | Clock stepped back 1 h and forward 1 h during away and during a hand override | Away and override end at the intended wall time; no negative durations in the log |
| T9 | DST change days with a switch point at 02:30 | The switch happens once, or is skipped, never twice |
| T10 | History partition: power loss mid-slot (truncated slot, bad CRC), mid-erase (half-erased sector), sequence wrap | Bad slots skipped; restore order by sequence; the newest record still found for the PI restore |
| T11 | Boiler never answers, then answers, then answers only "unknown data-id" | Link lost after 5 s (done today in `test_never_answered`), up on the first reply, unsupported IDs dropped from the poll list |
| T12 | Replies with wrong parity, wrong data-id, wrong message type | Counted as bad, retried once, never used as data |
| T13 | Control task stops calling `otbus_set_cmd` for 30 s | After H2: bus sends CH off. Today: keeps heating |
| T14 | Web: body longer than 12 288 bytes, missing fields, `%` at the end of a value, 600-character SSID | 400 or field ignored; no truncation that changes meaning |
| T15 | OTA: truncated image, image for another chip, upload stalled halfway, power cut during the write | Current firmware stays; the server is usable again within 30 s (fails today for the stall, see M4) |
| T16 | OTA trial with the router off | Decide the wanted result (M3), then test it |
| T17 | Brown-out during `save()` between the keys | Loaded state is consistent (away without an end time must not mean "away forever") |

---

## What looked good

- Control and bus tasks are pinned to core 1 at priorities 20 and 21, away from UI and Wi-Fi on core 0; the bus keeps its 1 s cadence without catching up after an overrun (`ot_bus.c:176-178`).
- Sensor loss, frost protection, clock loss and link loss each have a defined fallback and an alert.
- Every web handler calls `check_auth` (`webui.c:628-1500`); the password is stored salted and compared in constant time; the web interface stays closed until a login is set on the device (`webui.c:169-178`).
- Form values are copied with `strlcpy` and bounded buffers throughout `post_save`; HTML output is escaped (`webui.c:72-98`).
- OTA uses the bootloader rollback with a hardware check before the trial (`ota_check.c:82-85`).
- History writes are CRC-protected, sequence-numbered and wear-levelled across sectors.
- The pure-logic modules have 2 992 host checks that also pass under ASan and UBSan.

## Suggested order of work

1. H1 and H2 together (one config line plus a command timeout): closes the only "keeps heating blind" path.
2. H4 and H3: make heating independent of the display and of NVS defaults before the device goes on the wall.
3. H5, then M4 and M10: small web server changes.
4. M1: reset the C6 instead of the whole device.
5. M9: turn on stack checking and core dumps before the first real heating season, so the next unexplained restart leaves evidence.
6. H6 signed OTA images; M12 and HTTPS are larger and can follow.
7. Test extraction (sensor filter, Manchester decoder, history ring) and T1–T17.
