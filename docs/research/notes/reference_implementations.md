# Reference OpenTherm Thermostat Implementations: Control Logic, Safety, Message Loops, Lessons

Research date: 2026-11-15. Source code findings come from shallow clones taken that day: Alexwijn/SAT HEAD 7d43560 (2026-03-01), versatile_thermostat HEAD (2026-11-14), better_thermostat HEAD (2026-11-14), ihormelnyk/opentherm_library, jpraus/arduino-opentherm. GitHub file links point to the default branch, so line contents may drift.

## OpenTherm Gateway (OTGW, Schelte Bron): architecture, monitor mode, overrides, boiler quirks

### Takeaway
OTGW is a man-in-the-middle that decodes every frame and can rewrite it. It is the reference design for (a) passive logging (GW=0) and (b) safe external override: an external CS setpoint must be refreshed at least every minute or the gateway drops it, and a watchdog reset falls back to a hardware pass-through so the original thermostat keeps working.

### Cited Findings
- Normal mode is back-to-back. The gateway receives the whole frame, examines and possibly changes it, then forwards it. This adds about 70 ms per round trip. — [OTGW firmware docs](https://otgw.tclcode.com/firmware.html)
- GW=0 is monitor mode: "the received signal level is passed through to the output driver without any processing." It is mainly a diagnostic for communication problems. GW=1 is gateway mode (decode and modify). The setting is stored in non-volatile memory, and GW=R resets the gateway. — [OTGW firmware docs](https://otgw.tclcode.com/firmware.html)
- Fail-safe: after a watchdog reset the firmware falls back to pass-through, so the thermostat drives the boiler directly. FS=0 disables this. — [OTGW firmware docs](https://otgw.tclcode.com/firmware.html)
- Override commands:
  - TT is a temporary room setpoint that holds until the thermostat's next program change. TC is a constant room setpoint.
  - CS overrides the control setpoint (ID 1) sent to the boiler. CS=0 passes the thermostat's value through again. C2 does the same for circuit 2.
  - CH and H2 override the CH-enable bits in ID 0.
  - MM overrides max modulation, SH the max CH setpoint, SW the DHW setpoint.
  - OT supplies outside temperature. RT and BS supply room temperature and room setpoint (some heat pumps need them).
  - SM is summer mode, BW blocks DHW, VS and CL control ventilation and cooling.
  - RR is a remote request, PM a priority message, TP transparent parameters, RS a counter reset.
  - SR and CR set or clear a fixed response returned to the thermostat. AA and DA manage a table of up to 32 alternative Data IDs. UI and KI mark a Data ID as unsupported or known.
  — [OTGW firmware docs](https://otgw.tclcode.com/firmware.html)
- Vigilance rule: "A CS command with a value of 8 or higher must be repeated at least every minute as long as adjustment is needed." This prevents runaway heating if the controlling software dies. C2 has the same rule. — [OTGW firmware docs](https://otgw.tclcode.com/firmware.html)
- To stop heating via override, the docs say to send a low CS and clear CH. They also warn that manipulating these values "may severely impact the control algorithm of the thermostat." — [OTGW firmware docs](https://otgw.tclcode.com/firmware.html)
- Unsupported IDs: if the boiler answers Unknown-DataId three times in a row, the gateway treats the ID as unsupported and reuses that slot for its own requests ("available slots"). — [OTGW firmware docs](https://otgw.tclcode.com/firmware.html)
- After a receive error, the gateway waits for the bus to be idle, defined as no level change for at least 10 ms. — [OTGW firmware docs](https://otgw.tclcode.com/firmware.html)
- Thermostat disconnection is detected from line voltage outside the OpenTherm range and reported as an event. An on/off thermostat triggers this too. — [OTGW firmware docs](https://otgw.tclcode.com/firmware.html)
- Boiler and thermostat quirks:
  - Remeha thermostat models must be forced with FT=C (Celcia 20) or FT=I (iSense).
  - A boiler-reported ID 28 (return temperature) overrides an attached sensor unless UI=28 is set.
  - When the boiler does not support the DHW or max-CH bounds, the gateway simulates the ID 6 reads.
  - The ID 9 remote override setpoint resets to 0 once the thermostat stops honoring it.
  — [OTGW firmware docs](https://otgw.tclcode.com/firmware.html)

### Inferences
- For an ESP32 master, two OTGW patterns are worth copying.
  - A setpoint keep-alive and deadman: if the higher-level controller (HA, MQTT) stops refreshing, revert to a safe default within about 60 s.
  - Counting Unknown-DataId replies and dropping that ID from the poll loop after 3, so bus slots are not wasted.
- A hardware pass-through relay that reconnects the original thermostat on watchdog or brown-out is an established fail-safe pattern for hybrid installs.

### Gaps
- I did not fetch OTGW's per-boiler compatibility and FAQ pages, so specific boiler lockout or cycling stories from OTGW users are not captured.

## ESPHome OpenTherm component: polling, driving the boiler, PID, limitations

### Takeaway
ESPHome has a native OpenTherm hub that is master-only. It runs a constant message loop: status binary sensors refresh every cycle, and configuration flags are read once at init. The documented control path is ESPHome's generic `pid` climate writing the `t_set` (ID 1) output, with an external room sensor and a `ch_enable` switch. It knowingly drops some frames when other components disable interrupts.

### Cited Findings
- The hub needs `in_pin` and `out_pin`. The labels are reversed from the adapter: "your bridge's `in` pin becomes the hub's `out` pin and vice-versa." — [ESPHome OpenTherm](https://esphome.io/components/opentherm/)
- OpenTherm "transmits its messages in a constant loop." Status binary sensors update in every message cycle. Configuration-type binary sensors (for example `dhw_present`) are set only during initialization. — [ESPHome OpenTherm](https://esphome.io/components/opentherm/)
- Writable values exposed as outputs or numbers include `t_set`, `t_set_ch2`, `t_dhw_set`, `max_t_set`, `max_rel_mod_level`, and `cooling_control`. `ch_enable` and `dhw_enable` can be fixed at hub level or exposed as switches. — [ESPHome OpenTherm](https://esphome.io/components/opentherm/)
- Documented PID example:
  - A `t_set` output with `min_value: 20`, `max_value: 65`, and `zero_means_zero: true`.
  - A `climate: pid` with `heat_output: t_set`, `kp: 0.4`, and `ki: 0.004`.
  - The room temperature is pushed via a `heartbeat: 1s` filter "to update PID parameters."
  — [ESPHome OpenTherm](https://esphome.io/components/opentherm/)
- With `min_value` above 0 you "must" use the `ch_enable` switch to turn heating off. The PID output never reaches 0, so the boiler would otherwise never stop. — [ESPHome OpenTherm](https://esphome.io/components/opentherm/)
- `sync_mode` "prevents other components from disabling interrupts while we are talking to the boiler." It is recommended when invalid-response errors are intermittent (typical with Dallas 1-wire sensors). It "usually won't eliminate them entirely" and can block the main loop long enough to cause log warnings. — [ESPHome OpenTherm](https://esphome.io/components/opentherm/)
- `before_send` and `before_process_response` hooks can rewrite frames. The documented example remaps the DHW setpoint from ID 56 to ID 162 for a Daikin D2C boiler. — [ESPHome OpenTherm](https://esphome.io/components/opentherm/)
- Limitations:
  - The component is master-only; there is no gateway or slave mode.
  - The existing thermostat "is not usable" while ESPHome controls the boiler.
  - Special hardware is required.
  - Dropped frames are possible.
  — [ESPHome OpenTherm](https://esphome.io/components/opentherm/)
- Listed adapters: DIYLESS Master OpenTherm Shield, elvinhome THRM ONE, Ihor Melnyk's adapter, and Jiří Praus' shield. — [ESPHome OpenTherm](https://esphome.io/components/opentherm/)

### Inferences
- Using a plain PID straight onto flow setpoint, with no heating curve feed-forward, is the "minimum viable" approach. Projects like SAT exist mainly because this cycles badly at low load.
- Interrupt-latency sensitivity is a real hazard on ESP32 too. Generating Manchester timing with RMT, or a timer plus GPIO ISR on a dedicated core, is preferable to bit-banging with `delayMicroseconds`.

### Gaps
- The docs do not list the exact per-cycle message order or rate. That would need the `esphome/components/opentherm/hub.cpp` source, which I did not read.

## Ihor Melnyk opentherm_library (Arduino/ESP8266/ESP32)

### Takeaway
This is a simple interrupt-driven library. Transmission is bit-banged with `delayMicroseconds(500)` half-bits. Reception uses a pin-change ISR that samples at about 750 µs. Responses time out after 1 s, and the master waits 100 ms after each response before the next request is allowed. Responses are classified as SUCCESS, INVALID, or TIMEOUT.

### Cited Findings
- API:
  - Setup: `OpenTherm(inPin, outPin, isSlave)`, `begin(ISR, callback)`.
  - Requests: `sendRequest` (blocking), `sendRequestAsync` with `process()` in `loop()`, plus `isReady()`.
  - Frame builders: `buildRequest`, `buildSetBoilerStatusRequest(CH, DHW, cooling, OTC, CH2)`, `buildSetBoilerTemperatureRequest`.
  - High-level wrappers: `setBoilerStatus`, `setBoilerTemperature`, `setDHWSetpoint`.
  - Response decoding: `isFault`, `isCentralHeatingActive`, `isHotWaterActive`, `isFlameOn`, `isDiagnostic`, `getFloat`, `getUInt`.
  - Validation: `isValidResponse` and `parity`.
  — [OpenTherm.h](https://github.com/ihormelnyk/opentherm_library/blob/master/src/OpenTherm.h)
- `OpenThermResponseStatus` is `{NONE, SUCCESS, INVALID, TIMEOUT}`. — [OpenTherm.h](https://github.com/ihormelnyk/opentherm_library/blob/master/src/OpenTherm.h)
- Timing in `OpenTherm.cpp`:
  - `sendBit` drives each Manchester half-bit for `delayMicroseconds(500)`.
  - The receive ISR uses a 750 µs threshold between edges.
  - `process()` declares TIMEOUT when more than 1,000,000 µs pass without a response.
  - After a response, the state machine sits in DELAY for 100,000 µs as master, or 20,000 µs as slave, before returning to READY.
  - `activateBoiler()` holds the line idle for `delay(1000)` at startup.
  — [OpenTherm.cpp](https://github.com/ihormelnyk/opentherm_library/blob/master/src/OpenTherm.cpp)

### Inferences
- The 100 ms post-response delay matches the OpenTherm 2.2 spec's requirement of at least 100 ms between frames. The 1 s communication cycle is left to the application.
- Blocking `sendRequest` takes about 34 ms of TX, then waits up to 1 s. On ESP-IDF this should run in a dedicated task.
- `delayMicroseconds` bit-banging is jitter-prone under Wi-Fi interrupts, the same problem ESPHome works around with `sync_mode`. Hardware timers or RMT are more robust.

### Gaps
- Whether the library's examples poll status at 1 s or faster was not checked.

## Jiří Praus (jpraus) arduino-opentherm / OpenTherm regulator

### Takeaway
This is an open hardware shield (5 V, 3 A built-in supply) with both master and slave interfaces. It supports thermostat, monitor, and man-in-the-middle use, and is derived from the OTGW design.

### Cited Findings
- The shield implements OpenTherm 2.2. It has a slave interface (to the boiler), a master interface (to the thermostat), and a "5V 3A built-in power supply." It supports master, slave, and gateway modes and is "Based on otgw.tclcode.com." — [jpraus/arduino-opentherm README](https://github.com/jpraus/arduino-opentherm)
- Its author used it as a man-in-the-middle "OpenTherm regulator." — [README](https://github.com/jpraus/arduino-opentherm); [hackaday project](https://hackaday.io/project/162819-opentherm-regulator)
- ESPHome lists it as a supported adapter. — [ESPHome OpenTherm](https://esphome.io/components/opentherm/)

### Inferences
- There is no evidence that the user's task name "jpraus/ot-thermostat" exists as a separate repo; the library repo is `jpraus/arduino-opentherm`.

### Gaps
- I did not inspect the regulator's control logic or any performance data.

## DIYLESS Master/Slave shields and thermostat firmware

### Takeaway
The DIYLESS Master shield is a level-shifting OpenTherm interface. It is powered from the MCU's 3.3 V or 5 V rail and has polarity-independent boiler terminals. Example firmware (Arduino, ESPHome, Tasmota, Domoticz) uses Ihor Melnyk's library with GPIO 21 (in) and GPIO 22 (out) on an ESP32 D1-mini layout.

### Cited Findings
- The shield is powered from the microcontroller's GND and 3.3 V or 5 V pin, depending on the MCU. — [DIYLESS Master OpenTherm Shield (mirror)](https://diyless-old.duckdns.org/product/master-opentherm-shield)
- Boiler terminals: "Connect B1/B2 terminals to the boiler (polarity does not matter)." — [DIYLESS ESP32 Thermostat Shield (mirror)](https://diyless-old.duckdns.org/product/esp8266-thermostat-shield); [Tindie listing](https://www.tindie.com/products/embeddedloop/diyless-master-opentherm-shield)
- The ESPHome sample uses GPIO 21 (in) and GPIO 22 (out) on a WeMos D1 mini ESP32. — [DIYLESS ESPHome OpenTherm Thermostat (mirror)](https://diyless-old.duckdns.org/blog/esphome-opentherm-thermostat)
- The DIYLESS OpenTherm Gateway "can't power a thermostat" and supports only low-power mode (no power-stealing passthrough). — [DIYLESS Gateway (mirror)](https://diyless-old.duckdns.org/product/esp8266-opentherm-gateway)
- The Laxilef OTGateway wiki maintains a comparison of OT adapters. — [OTGateway wiki: OT adapters](https://github.com/Laxilef/OTGateway/wiki/OT-adapters)

### Inferences
- Polarity independence comes from a bridge rectifier on the slave-facing side. The DIYLESS page does not state this; it is an inference. The OpenTherm spec itself requires polarity-free wiring.
- The MCU is not powered from the OpenTherm line (no power-stealing), so a separate supply is needed.

### Gaps
- The search returned a `diyless-old.duckdns.org` mirror rather than diyless.com. Content should be re-verified on the official site.

## Smart Autotune Thermostat (SAT, Alexwijn/SAT): exact algorithms from source

### Takeaway
SAT computes the flow setpoint as heating-curve feed-forward plus PID trim, and the PID gains scale automatically with the curve value. When the computed setpoint falls below what the boiler can deliver at minimum modulation, it switches to a duty-cycle PWM (on/off) mode with cycle limits. The minimum setpoint comes from a calibration (OPV) or is learned dynamically from return temperature and modulation. It also includes a flame-health classifier that detects short-cycling and stuck states.

### Cited Findings: setpoint composition
- `setpoint = max(heating_curve.value + pid.output, MINIMUM_SETPOINT)`, with MINIMUM_SETPOINT = 10 °C and MAXIMUM_SETPOINT = 65. — [climate.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/climate.py); [const.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/const.py)
- Heating curve:
  - `calc = 4*(T_target-20) + 0.03*(T_out-20)^2 - 0.4*(T_out-20)`
  - `value = base_offset + (coefficient/4)*calc`
  - `base_offset` is 27.2 °C for radiators and 20 °C for underfloor.
  - The default coefficient is 2.0.
  - Worked example (my arithmetic): radiators, target 20 °C, outside 0 °C gives 37.2 °C; outside −10 °C gives 46.7 °C.
  — [heating_curve.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/heating_curve.py); [const.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/const.py)
- Curve autotune: SAT back-calculates the coefficient implied by the achieved setpoint, applies a fuzzy correction of ±0.1, 0.2, or 0.3 depending on the derivative, and keeps a mean of the last 5. — [heating_curve.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/heating_curve.py)
- The docs offer three curve variants (Classic, Quantum, Precision; Precision is recommended). Suggested max setpoint is 55–75 °C for radiators and 50 °C for underfloor. — [SAT README](https://github.com/Alexwijn/SAT)

### Cited Findings: PID and automatic gains (controller version 3, the default)
- Automatic gains (on by default):
  - `kp = coefficient * heating_curve_value / 3` for radiators (divide by 4 for underfloor).
  - `ki = kp / 8400`.
  - `kd = 0.07 * 8400 * kp`, which is 588·kp.
  - Older versions 1 and 2 use different constants: aggression 99 or 81.5 for radiators and 73 for underfloor, with `ki = hc/73900` (v1) or `agv*hc/7200` (v2).
  — [pid.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/pid.py)
- Worked example (my arithmetic): coefficient 2 and curve 37.2 give kp ≈ 24.8, ki ≈ 0.00295 /s, and kd ≈ 14,600.
- Deadband = 0.1 °C:
  - The integral accumulates only when |error| ≤ deadband, as `integral += ki*error*dt`.
  - The integral resets when the error crosses into the deadband from outside.
  - The integral is clamped to ±heating_curve_value.
  - The derivative is used only when |error| > deadband.
  - The derivative is double-EMA filtered (α1 = 0.8, α2 = 0.6).
  — [pid.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/pid.py); [const.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/const.py)
- Manual-gain defaults: Kp 45, Ki 0, Kd 6000. Other defaults: sample time 1 min, `automatic_gains_value` 2.0, derivative time weight 2.5, and target step 0.5 °C. — [const.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/const.py)
- The README recommends room sensors with 2-decimal resolution and a 30 s refresh. — [SAT README](https://github.com/Alexwijn/SAT)

### Cited Findings: overshoot protection (OPV = lowest flow temperature the boiler holds at minimum modulation)
- The docs say SAT sends "MM=0 and CS=75," looks for "the highest flow water temperature the boiler can produce while running at 0% modulation," and takes at least 20 minutes. All TRVs should be open during calibration. If the boiler ignores 0% modulation, SAT falls back to a "more sophisticated" calibration. — [SAT README](https://github.com/Alexwijn/SAT)
- The current code differs from the docs:
  - Calibration setpoints are 62 °C (radiators), 45 °C (underfloor), and 40 °C (heat pump), each capped at the boiler's max.
  - Every 15 s the code sends CH on, the setpoint, and max relative modulation = 100 (MAXIMUM_RELATIVE_MODULATION).
  - It waits for flame (5 min timeout).
  - It then waits for a stable flow temperature (15 min timeout): above the starting temperature, with |T − EMA| ≤ 0.1 (EMA α = 0.5).
  - While the boiler reports modulation > 0, it tracks the setpoint to the current flow temperature and waits another 5 min.
  - Result: OPV = the stable temperature if modulation is 0. Otherwise OPV = `(100 − modulation)/100 × setpoint`.
  - On cancel it sends CH off and a 10 °C setpoint.
  — [overshoot_protection.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/overshoot_protection.py); [const.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/const.py) (contradiction with [README](https://github.com/Alexwijn/SAT) regarding MM=0 / CS=75)

### Cited Findings: PWM (low-load on/off) mode
- Trigger: PWM engages when the minimum setpoint (OPV, configured, or dynamic) exceeds the calculated setpoint. It can also be forced on. — [climate.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/climate.py)
- Duty fraction: `d = (requested_setpoint − base_offset) / (boiler_flow_temp − base_offset)`, clamped to 0..1. The flow temperature is EMA-smoothed (α 0.2) during the first 180 s of each ON period. — [pwm.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/pwm.py)
- Automatic duty cycle, with the default 4 cycles per hour:
  - Constants: base period 3600/4 = 900 s; minimum ON or OFF time 180 s; low threshold 180/900 = 0.2; high threshold 0.8.
  - d < 0.1: OFF for 1800 s. If the flame is already lit (and not DHW), run 180 s ON then 1620 s OFF.
  - d ≤ 0.2: ON 180 s, OFF = 180/d − 180.
  - d ≤ 0.8: ON = 900·d, OFF = 900·(1−d).
  - d ≤ 0.9: OFF 180 s, ON = 180/(1−d) − 180.
  - Above 0.9: continuously on (ON 1800 s, OFF 0).
  - An ON period is skipped if the computed ON time is under 180 s.
  - Each new ON increments a cycle counter, and no new ON starts after `cycles_per_hour` within the rolling hour.
  - The non-automatic duty cycle uses a fixed 13 min period by default.
  — [pwm.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/pwm.py); [const.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/const.py)
- The README describes this as on/off times in 15-minute intervals, extendable "up to 30 minutes" in mild weather. — [SAT README](https://github.com/Alexwijn/SAT)
- Setpoint during an ON pulse:
  - Once the flame has been on for more than 6 s (outside pump-start and overshoot handling), the target is flow − 3 °C, ramped in 1 °C steps. This makes the boiler stay at minimum modulation instead of overshooting.
  - If the flame is off and the average on-time is under 60 s, the setpoint is forced to flow + 10 °C to get ignition.
  - PWM disables itself if the boiler is cooling down and the requested setpoint rises above the PWM setpoint.
  — [pwm.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/pwm.py); [setpoint_adjuster.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/setpoint_adjuster.py)

### Cited Findings: dynamic minimum setpoint (experimental, off by default)
- In "normal" (non-PWM) mode the minimum is `(1 − modulation/100) × flow − 3`, clamped with a 30 °C bound. Normal mode means PWM is idle, modulation > 0 and unchanged for more than 180 s, and flow within ±1 °C of the setpoint.
- Otherwise the minimum is `configured_min + (return − base_return) × factor`. The factor defaults to 0.2; the docs recommend 0.1–0.5. `base_return` is the return temperature captured during warm-up and persisted.
  — [minimum_setpoint.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/minimum_setpoint.py); [const.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/const.py); [SAT README](https://github.com/Alexwijn/SAT)

### Cited Findings: flame and cycle health detection
- Constants:
  - At most 8 cycles per hour in PWM mode and 3 in non-PWM mode.
  - STUCK_OFF after 300 s without flame under demand (30 s for DHW).
  - STUCK_ON after 120 s of flame without demand.
  - Windows: 15 min for duty, 1 h for cycle count, 4 h for the median on-time.
  - At least 3 samples before judging.
  — [flame.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/flame.py)
- Classification:
  - In PWM mode: SHORT_CYCLING if cycles/h > 8. PWM_SHORT if the median on-time is less than 0.9 × the planned ON time.
  - For a modulating boiler: HEALTHY if cycles/h ≤ 2 and the 15-min duty is at least 0.48. SHORT_CYCLING if the median on-time is under 162 s and cycles/h > 3.
  — [flame.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/flame.py)
- The boiler state tracker labels phases: preheating, heating_up, near/at_setpoint, cooling_down, anti_cycling, pump_starting, waiting_for_flame, and overshoot_handling/stabilized. — [const.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/const.py); [boiler.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/boiler.py)

### Cited Findings: areas and multi-room
- When any room TRV calls for heat, the boiler starts.
- Heating mode "Comfort" (the default) uses the highest error across rooms as the PID error. "Eco" uses only the main thermostat's error.
- A primary thermostat can sync setpoint and `hvac_action` and acts as a backup if HA fails.
- Window sensors and presets are supported.
  — [SAT README](https://github.com/Alexwijn/SAT); [const.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/const.py)

### Inferences
- SAT's central idea for a firmware port: feed-forward plus trim (curve + PID), a learned or calibrated minimum flow, then time-proportional on/off below that minimum, while clamping the CH setpoint just under the actual flow so the boiler sits at minimum modulation.
- The OPV and dynamic-minimum logic needs the boiler to report ID 17 (relative modulation), ID 25 (flow), and ID 28 (return). Boilers that don't support these degrade to the configured minimum.
- Gains are expressed in °C of flow setpoint per °C of room error. The very large Kd (about 15,000 s) is offset by the derivative being active only outside the 0.1 °C deadband and heavily filtered.

### Gaps
- I did not trace the "more sophisticated" fallback calibration path in the code.
- I did not inspect the Precision and Quantum curve variants (Desmos only); the code shows only the single formula above.
- I found no published measured performance data for SAT, such as cycle counts before and after. The HA community thread was not reviewed.

## Better Thermostat and Versatile Thermostat (HA): TRV calibration, TPI, central boiler

### Takeaway
Versatile Thermostat (VT) uses TPI: `on% = coef_int·(Tset−Troom) + coef_ext·(Tset−Tout)`, clamped 0–1, applied over a cycle. It now has Auto-TPI learning and a central-boiler manager that turns the boiler on when the number of active VTherms or their total power exceeds a threshold, with activation delay and keep-alive. Better Thermostat (BT) wraps TRVs with external-sensor calibration. As of Nov 2026 its modes are Default, Aggressive, Heating-Power, MPC, MPC v2, TPI, PID, and None.

### Cited Findings
- VT TPI: `calculated_on_percent = tpi_coef_int*delta_temp + tpi_coef_ext*delta_ext_temp`, clamped to [0,1] and then to `max_on_percent`. Optional low and high thresholds force 0 depending on slope. Defaults: safety on-percent 0.5 minimum and 0.1 fallback. — [prop_algo_tpi.py](https://github.com/jmcollin78/versatile_thermostat/blob/main/custom_components/versatile_thermostat/prop_algo_tpi.py); [const.py](https://github.com/jmcollin78/versatile_thermostat/blob/main/custom_components/versatile_thermostat/const.py)
- VT includes Auto-TPI (`auto_tpi_manager.py`, `tpi_identification.py`) and a PI algorithm, along with heating-failure detection, safety, window, and presence managers. — [VT source tree](https://github.com/jmcollin78/versatile_thermostat/tree/main/custom_components/versatile_thermostat)
- VT central boiler:
  - The boiler is active if `nb_active_devices` exceeds a threshold or `total_power_active` exceeds a threshold.
  - The configurable `activation_delay_sec` is cancelled if the condition clears.
  - `keep_alive_boiler_delay_sec` (default 0) re-sends the state periodically.
  — [feature_central_boiler_manager.py](https://github.com/jmcollin78/versatile_thermostat/blob/main/custom_components/versatile_thermostat/feature_central_boiler_manager.py)
- BT CalibrationMode enum: DEFAULT, fix_calibration (aggressive), heating_power_calibration, no_calibration, mpc_calibration, mpc_v2_calibration, tpi_calibration, and pid_calibration. The MPC helper adapts gain and loss estimates (methods "insufficient_heat_boost", "heat_rate", "high_u_ss", "recovery"). — [BT const.py](https://github.com/KartoffelToby/better_thermostat/blob/master/custom_components/better_thermostat/utils/const.py); [BT mpc.py](https://github.com/KartoffelToby/better_thermostat/blob/master/custom_components/better_thermostat/utils/calibration/mpc.py)

### Inferences
- Neither project is an OpenTherm controller. They produce per-room demand (on% or valve position) that can be aggregated into a boiler on/off or setpoint. A firmware could expose "demand %" over MQTT so these HA tools can drive it.
- TPI is the right model for an on/off fallback mode (OpenTherm unavailable → relay mode).

### Gaps
- I did not verify TPI default coefficients from source (commonly cited as coef_int 0.6 and coef_ext 0.01 in VT docs), nor the default cycle length.
- I did not read BT's calibration math in detail (target-temperature vs. offset based).

## Commercial controllers: tado, Nest, evohome, Netatmo, Vaillant, Bosch

### Takeaway
All the modulating commercial products follow the same pattern: compute a flow setpoint (ID 1) from room error and, usually, outdoor temperature, add learned preheat timing, and cap max flow. Exact algorithms are proprietary. Users repeatedly complain that products leave max modulation at 100% and only move the setpoint, causing low-load short-cycling.

### Cited Findings
- **Nest:**
  - With True Radiant on, the thermostat sets the boiler's target water temperature over OpenTherm and the boiler modulates.
  - With it off, 3rd-gen Nest starts and stops at the fixed supply temperature from Pro Setup instead of changing it dynamically.
  - True Radiant learns how long the system takes to heat, starts early, and stops early to avoid overshoot. Max Duration caps night preheat.
  - Early-On is the equivalent for forced-air systems.
  — [Google: True Radiant](https://support.google.com/googlenest/answer/9250165); [Google: Early-On](https://support.google.com/googlenest/answer/9246532)
- **tado:**
  - "Flow Temperature Optimization" is OpenTherm-only (not eBUS) on V2, V3, V3+, and X devices.
  - A static max flow is free. A dynamic heat curve, where the max applies only on cold days, needs the Auto-Assist subscription.
  - Users observe the cap being lowered with the weather, for example 70 → 63 °C.
  — [tado community announcement](https://community.tado.com/en-gb/discussion/24396/announcement-flow-temperature-optimization-now-available-for-v-generations-and-tado-x-opentherm); [tado community: does tado modulate](https://community.tado.com/en-gb/discussion/24885/does-tado-modulate-the-flow-temperature)
- **Honeywell evohome:**
  - Boiler demand goes either as TPI (on/off via a BDR relay) or modulated via the OpenTherm bridge.
  - Per community reverse-engineering, zone demand comes from the highest TRV valve position and boiler demand from the highest zone demand.
  - Resideo markets "advanced load scaling" that learns zone heat capacity to keep the boiler at its lowest rate.
  - Users report demand jumping to 100% whenever any zone hits 100%, and incompatibility with old-OT-version boilers.
  — [HA community evohome thread](https://community.home-assistant.io/t/honeywell-ch-dhw-via-rf-evohome-sundial-hometronics-chronotherm/151584?page=90); [Evohome protocol 3150](https://github.com/zxdavb/The-Evohome-Protocol/wiki/3150:-Zone-Heat-Demand); [Resideo evohome](https://homecomfort.resideo.com/sites/Europe/en-gb/Solutions/Smart_Home_System/Pages/evohome.aspx)
- **Netatmo:**
  - Netatmo's modulation algorithms use a heating curve that sets the water setpoint from the house's thermal performance and outdoor temperature.
  - Auto-Adapt uses outdoor temperature and insulation to decide the heating start time.
  - A user reports that it always sends 100% max modulation and only adjusts the setpoint, causing short-cycling at low demand. This is a feature-request post, not official.
  — [Netatmo help: OpenTherm modulation algorithms](https://helpcenter.netatmo.com/hc/en-us/articles/360017286720-How-do-the-OpenTherm-modulation-algorithms-work) (returned 403 on fetch; content per search snippet); [Netatmo feature request](https://helpcenter.netatmo.com/hc/en-us/community/posts/33120747285266)
- **Vaillant sensoCOMFORT:**
  - Flow temperature follows an outdoor heating curve with slope and level settings.
  - An "adaptive heating curve" option exists, but no official description of its learning method was found.
  - Heat Geek suggests a starting slope of about 0.6–0.8.
  — [Vaillant sensoCOMFORT](https://vaillant.com/home/products/weather-compensated-system-control-sensocomfort-11039.html); [Heat Geek](https://www.heatgeek.com/vaillant-sensocomfort-heat-pump-controller)

### Inferences
- The common commercial recipe is weather-compensated curve + room trim + learned preheat + user-capped max flow. That is essentially SAT's architecture.
- Explicitly managing ID 14 (max relative modulation) is a differentiator, since several products reportedly don't.

### Gaps
- tado "Early Start" learning and Smart Schedule internals: no technical source found.
- Bosch EasyControl (uses EMS, not OpenTherm, on Bosch/Worcester): not researched.
- Vaillant adaptive curve algorithm and evohome optimisation: no primary technical documentation found.
- Patents were not searched.

## Other OSS (Laxilef OTGateway, EMS-ESP, etc.) and common failure modes

### Takeaway
Laxilef's OTGateway (ESP32/ESP8266) is the most complete standalone OSS OpenTherm master firmware. It offers equitherm (weather curve) plus PID plus hysteresis, an emergency mode on loss of Wi-Fi, MQTT, or sensors, and remote fault reset. The recurring failure themes are frame errors from interrupt latency, boilers not supporting IDs or old OT versions, boilers that ignore setpoints or modulation limits, and low-load short-cycling.

### Cited Findings
- OTGateway features:
  - Heating control by PID and equitherm ("adjusts the temperature based on indoor and outdoor temperatures"), with hysteresis.
  - External Dallas, NTC, or BLE sensors.
  - In "any dangerous situation (loss of connection with Wifi, MQTT, sensors, etc)" the system "will not let you and your home freeze."
  - Remote fault reset "not with all boilers."
  - The wiki has adapter and compatibility pages.
  — [Laxilef/OTGateway](https://github.com/Laxilef/OTGateway)
- Failure mode, frame errors: intermittent invalid responses when other drivers (for example Dallas) disable interrupts. — [ESPHome OpenTherm](https://esphome.io/components/opentherm/)
- Failure mode, unsupported IDs: handled by detecting three consecutive Unknown-DataId replies. — [OTGW firmware docs](https://otgw.tclcode.com/firmware.html)
- Failure mode, protocol version: an OT bridge with an incompatible OT version. — [HA community evohome thread](https://community.home-assistant.io/t/honeywell-ch-dhw-via-rf-evohome-sundial-hometronics-chronotherm/151584?page=90)
- Failure mode, boiler ignores MM=0: SAT needs an alternate calibration. — [SAT README](https://github.com/Alexwijn/SAT)
- Failure mode, short-cycling at low load because modulation is always 100% and only the setpoint is varied. — [Netatmo feature request](https://helpcenter.netatmo.com/hc/en-us/community/posts/33120747285266)
- Failure mode, PID never turns the boiler off when the output minimum is above 0. — [ESPHome OpenTherm](https://esphome.io/components/opentherm/)
- Power: master shields are MCU-powered and polarity-free. Gateways cannot power a thermostat. — [DIYLESS (mirror)](https://diyless-old.duckdns.org/product/esp8266-opentherm-gateway)

### Inferences
- A robust ESP-IDF master should include:
  - Hardware-timed Manchester I/O.
  - A ≤1 s message loop that always includes ID 0 (status) and ID 1 (setpoint).
  - Per-ID unsupported tracking.
  - A response-timeout counter that triggers a safe state. Boilers typically drop to their own fallback when they see no valid master frames for a while; that spec detail is not verified here.
  - A setpoint deadman for external control.
  - Explicit min on and off times or a cycles-per-hour cap.
  - Using ID 14 and the measured min-modulation flow (an OPV-like calibration) to avoid short-cycling.

### Gaps
- EMS-ESP: not researched (different bus).
- I found no quantitative performance comparisons (energy savings, cycle reduction) across these projects.
- No concrete boiler lockout case studies with fault codes were collected.
- The "power-stealing" theme was only lightly covered.
