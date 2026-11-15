# OpenTherm Protocol: Technical Reference for a DIY Master (Room Thermostat)

Source legend used throughout:
- **[SPEC v2.2]** = OpenTherm Protocol Specification v2.2 (OpenTherm Association, 35 pages; the 2003 revision is the widely mirrored public one). Text was extracted directly from the PDF mirror at http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf (also mirrored at http://files.domoticaforum.eu/uploads/Manuals/Opentherm/Opentherm%20Protocol%20v2-2.pdf, linked from Wikipedia). Page numbers refer to that PDF.
- **[v4.x via implementations]** = IDs/bits defined after v2.2 (v3.0/v4.x), which are NOT publicly published; taken from open-source implementations that claim to follow v4.2: ihormelnyk OpenTherm.h (https://github.com/ihormelnyk/opentherm_library/blob/master/src/OpenTherm.h) and OTGW-firmware by rvdbreemen (https://github.com/rvdbreemen/OTGW-firmware, files src/OTGW-firmware/OTGW-Core.h and OTGW-Core.ino).
- **[COMMUNITY]** = forum posts, gateway docs, integration docs – observations, not normative.

---

## 1. Physical layer (current loop, encoding, bit timing, frame)

### Takeaway
Two-wire, polarity-free point-to-point line, powered by the boiler (slave). Master→boiler signals by switching line **voltage** (≤7 V idle/low, 15–18 V high); boiler→master signals by switching loop **current** (5–9 mA idle/low, 17–23 mA high). Manchester (bi-phase-L), 1000 bit/s, 34 bit times per frame (start '1' + 32-bit frame + stop '1'), even parity over all 32 bits.

### Cited Findings
- Medium: 2 wires, untwisted pair (twisted/screened may be needed in noisy environments), max line length 50 m, max cable resistance 2 × 5 Ω, polarity-free ("interchangeable") — [SPEC v2.2 §3.1, p.12](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- "The system operates by sending current signals from the boiler unit to the room unit and voltage signals in the reverse direction." Idle state is "a low-power consumption state i.e. low current and low voltage." — [SPEC v2.2 §3.2, p.12](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Boiler→room (current): Ihigh 17..23 mA; Ilow 5..9 mA (idle); max open-circuit voltage 42 Vdc; rise/fall time 50 µs max (typ 20 µs). Room-unit receive threshold Ircv 11.5..14.5 mA, nominal 13 mA — [SPEC v2.2 §3.2.1, p.13](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Room→boiler (voltage): Vhigh 15..18 V; Vlow 7 V max (idle); rise/fall 50 µs max (typ 20 µs). Boiler receive threshold Vrcv 9.5..12.5 V, nominal 11 V — [SPEC v2.2 §3.2.2, p.14](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Power feeding: boiler powers the line; guaranteed current available to the room unit is the minimum idle current, so "Room unit current consumption: 5 mA max." Nominal 3.3 V achievable at the room unit with a low-cost interface. Room unit may alternatively be locally powered (mains/batteries). Room unit "shall provide the functionality to operate regardless of polarity". Boiler interface provides safety isolation from mains (EN60730-1) — [SPEC v2.2 §3.3, p.15](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Encoding: "Manchester / Bi-phase-L. Bit value '1': active-to-idle transition. Bit value '0': idle-to-active transition." (i.e. '1' = high in first half, low in second half; '0' = low then high). Manchester validity must be checked by the receiver and the frame rejected on error — [SPEC v2.2 §3.4.1, §3.4.3, p.15–17](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Bit rate 1000 bit/s nominal; period between mid-bit transitions 900 µs..1150 µs (nominal 1 ms, i.e. −10%/+15%); "Timing should be reset on each transition so that any timing errors do not accumulate" — [SPEC v2.2 §3.4.2, p.16](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Frame: "Data is transmitted in 32 bit frames, with an added start bit ('1') at the beginning and stop bit ('1') at the end." Layout MSB-first: `P(1) | MSG-TYPE(3) | SPARE(4) | DATA-ID(8) | DATA-VALUE(16)`. Identical in both directions — [SPEC v2.2 §4.2, p.18](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Parity: "set or cleared such the total number of '1' bits in the entire 32 bits of the message is even." Spare bits "should always be '0'" — [SPEC v2.2 §4.2.1, §4.2.3, p.18–19](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Community library: "Controller(Arduino/ESP8266) input pin should support interrupts"; levels "(7..15V) still much higher than Arduino/ESP8266 levels, which requires OpenTherm Adapter" — [ihormelnyk/opentherm_library README](https://github.com/ihormelnyk/opentherm_library)
- Wikipedia summary: "The minimum available power is 35 mW. When using OpenTherm Smart Power this can, by primary request, also be 136 mW (medium power) or 255 mW (high power)." — [Wikipedia: OpenTherm](https://en.wikipedia.org/wiki/OpenTherm)

### Inferences
- A frame takes 34 ms on the wire (34 bits × 1 ms). Decoder should resync on every mid-bit edge and accept 0.9–1.15 ms between mid-bit edges; half-bit edges arrive ~0.5 ms after.
- Because the room unit is only guaranteed 5 mA in standard (low) power, a line-powered master with an ESP32 + camera is not viable; the project's master must be locally powered (spec explicitly allows this, §3.3.1).
- The master's receiver must work with either polarity (bridge rectifier in the adapter); typical DIY adapters handle this.

### Gaps
- Exact electrical definition of Smart Power medium/high modes (current levels, voltage) is in the unpublished v3.0+ spec; only power figures (35/136/255 mW) were found via Wikipedia.

---

## 2. Timing rules, start-up, and communication loss

### Takeaway
Master sends one request per conversation; slave replies 20–800 ms after the end of the request; master waits ≥100 ms after the conversation ends before the next request; master must communicate at least every 1 s (+15% → 1.15 s max). If the boiler sees no valid frame for 5–15 s while the line is at Vlow, it treats that as a **short-circuit = heat demand** (installer test feature). Master falls back to OT/- after 20 s of no response at start-up; in OT/+ mode, no valid response for 1 min → "communications fault" state.

### Cited Findings
- "The slave is expected to respond with a single frame reply within a defined period of 20ms to 800ms from the end of the master transmission. The master unit must wait 100ms from the end of a previous conversation before initiating a new conversation. The master must communicate at least every 1 sec (+15% tolerance)." Diagram: "Maximum: 1.15 sec" — [SPEC v2.2 §4.3.1, p.19](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Frame error handling: "In all cases of errors being detected in the incoming frame, the partial frame is rejected and the conversation should be terminated. No errors are treated as recoverable. If the slave does not respond, then the master should note that the conversation is incomplete. If a conversation is terminated the master should re-attempt the same conversation at the next appropriate scheduled time for communications." — [SPEC v2.2 §4.5, p.21](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- OT/+ vs OT/- auto-detection in the master: "On power-up or after the physical connection is made, the room unit tries to communicate using OT/+ messages. If the boiler controller does not respond to one of these messages after 20 seconds, then the room unit switches to using OT/- signalling. An OT/+ boiler controller must start communications within this 20 second period..." State diagram: TRYING OT/+ → (no valid response 20 s) → OT/- MODE; TRYING → (valid response) → OT/+ MODE; OT/+ MODE → (no valid response for 1 min) → COMMUNICATIONS FAULT → (valid response) → OT/+ MODE — [SPEC v2.2 §2.3, p.9–10](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Short-circuit feature: "The boiler unit must support an important installation feature which allows the terminals at the boiler to be short-circuited to simulate a heat demand... The boiler unit should interpret the short-circuit as a heat demand within 15 secs ... It is allowable that this can implemented by a software-detection method. The software short-circuit condition is defined as a low-voltage state (Vlow) with no valid communications frame for at least 5 seconds." State diagram: no valid frame for 5..15 s → SHORT-CIRCUIT STATE; valid communications re-started → COMMUNICATIONS ACTIVE — [SPEC v2.2 §3.5, p.17](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- OTGW docs: "The boiler generally answers within 200ms"; gateway adds about 70 ms; "In stand-alone mode, the OTGW normally sends one message per second"; MI command sets interval 100–1275 ms because "The specifications define that there must be a gap of at least 100 ms"; error recovery waits until "no level change has occurred in at least 10 ms" — [OTGW firmware docs](https://otgw.tclcode.com/firmware.html) [COMMUNITY]
- ihormelnyk library implementation: response timeout of 1 s (`(newTs - ts) > 1000000` µs) and a 100 ms delay after a received response before the master may send again (`isSlave ? 20000 : 100000`) — [OpenTherm.cpp](https://github.com/ihormelnyk/opentherm_library/blob/master/src/OpenTherm.cpp) [COMMUNITY]
- Boiler behaviour on comm loss (community reports, model-specific): a user reported that after the OTGW lost power the boiler showed error 95 ("set to obey the OpenTherm bus with no room unit connected") and ran CH at a fixed 60 °C setpoint; a gateway post says the OTGW raises the line to Vhigh when the thermostat stops sending, because the boiler may interpret an idle line lasting >5 s as heat demand — search summaries of [domoticaforum "Boiler runaway when removing OTGW power"](https://www.domoticaforum.eu/viewtopic.php?p=100938) (page itself returned HTTP 403 to fetch; content seen only via search snippets) [COMMUNITY]
- Vendor error codes for lost OT comms reported by users: Baxi E87, Nest H73; Intergas Comfort Touch shows a warning — search results incl. [tado community Baxi E87](https://community.tado.com/en-gb/discussion/21391/baxi-error-e87), [Intergas Comfort Touch](https://www.intergas-verwarming.nl/?p=8912) [COMMUNITY]
- OTGW's own vigilance rule for externally-set control setpoints: "A CS command with a value of 8 or higher must be repeated at least every minute as long as adjustment is needed" — described as "a vigilance check to prevent runaway heating" — [OTGW firmware docs](https://otgw.tclcode.com/firmware.html) [COMMUNITY]

### Inferences
- **Critical design hazard:** the idle line state (Vlow) is electrically the same as a short circuit. If the master firmware crashes/hangs with the line idle, a spec-compliant boiler will, after 5–15 s, treat it as a **maximum heat demand** (like an on/off thermostat closing). A safe master should therefore (a) use a hardware watchdog so comms resume quickly, and (b) consider driving the line to Vhigh (not idle) when it deliberately stops talking — this mirrors OTGW behaviour (community) and in OT/- terms is a 0% "low" duty cycle (<5% = CH disabled). This is an inference; boiler behaviour with a permanently high line is not specified for OT/+ boilers.
- There is no spec-defined "fall back to the boiler's own setpoints after N seconds" for OT/+; what the boiler does after losing comms (beyond the short-circuit rule) is manufacturer-specific (fixed setpoint, error code, lockout, or stop).
- Practical cycle: one conversation ≈ 34 ms request + 20–800 ms (typ <200 ms) reply + 34 ms reply frame + 100 ms gap ≈ 250–400 ms typical, so 2–4 conversations/s is possible; 1 message/s (OTGW stand-alone default) is the conservative norm.

### Gaps
- Boiler-side timeout and fallback behaviour for OT/+ comm loss is not specified in v2.2; v4.x may add rules but the spec is unpublished. Manufacturer manuals per model are the only authoritative source.
- Could not read the domoticaforum threads directly (HTTP 403); claims from them are from search snippets.

---

## 3. OT/+ vs OT/- (OpenTherm Lite), Smart Power, polarity

### Takeaway
OT/+ = digital Manchester protocol (the only level that may carry the OpenTherm logo). OT/- (Lite) = same physical layer, master sends a 100–500 Hz PWM voltage whose duty cycle maps to the control setpoint; boiler signals lock-out by holding high current. Smart Power (v3.0, 2008) lets the master ask for more line power (low/medium/high). Connection is polarity-free.

### Cited Findings
- OT/+ "provides a digital communications system for data-exchange between two microprocessor-based devices"; OT/- "uses a PWM signal and simple signalling capabilities to allow implementation on analogue-only products." Both share the physical layer — [SPEC v2.2 §1.2, p.6](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Products conforming only to OT/- cannot carry the OpenTherm logo — [SPEC v2.2 §2.3, p.9](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- OT/- master→boiler: PWM with "Duty Cycle (%) = tlow / tperiod ie. the % time over the period that the line is low", frequency 100–500 Hz (2 ms < tperiod < 10 ms), 0–100%. Boiler→master: high current = boiler lock-out fault, may be held permanently — [SPEC v2.2 §6.1–6.2, p.34](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- OT/- data equivalence: Tset (id=1), CH-enable (id=0 bit 0 master), lock-out fault (id=0 slave bit 0). Duty-cycle→Tset mapping graph: 10%..90% duty maps linearly onto setpoint range; "A Duty-Cycle less than 5% is used to indicate a CH-disabled state"; tolerance ±2% — [SPEC v2.2 §6.3, p.35](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf) (graph endpoints in °C were not extractable from the PDF text)
- OT/- is "rarely used" — [Wikipedia: OpenTherm](https://en.wikipedia.org/wiki/OpenTherm) (tagged citation needed)
- Smart Power: "On June 16, 2008, OpenTherm specification 3.0 was approved"; "The primary can request the secondary to change the available power to low, medium or high power", allowing features such as a backlight — [Wikipedia: OpenTherm](https://en.wikipedia.org/wiki/OpenTherm)
- Smart Power signalling (reverse-engineered, not official): OTGW author observed that if the thermostat keeps its output inverted for >20 ms the gateway treats it as High power, shorter → Medium; poster notes timings "may not match the official Smart Power specification"; another forum user states OT specs ≥3.0 are not published — search summary of [domoticaforum "OT 3.0 Smart Power"](https://domoticaforum.eu/viewtopic.php?p=92515) (HTTP 403 on direct fetch) [COMMUNITY]
- Smart Power is negotiated via ID 2 (master config): "If the thermostat specifies Smart Power support the gateway will acknowledge it, regardless of the boiler response." Power-mode events low/medium/high; `PR=P` reports "P=L" / "P=M" / "P=H" / "P=N" — [OTGW firmware docs](https://otgw.tclcode.com/firmware.html); [OTGW-Core.ino line ~664](https://github.com/rvdbreemen/OTGW-firmware) [COMMUNITY]
- Polarity: room unit "shall provide the functionality to operate regardless of polarity of the line signal" — [SPEC v2.2 §3.3.2.3, p.15](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)

### Inferences
- For a locally powered DIY master, Smart Power is irrelevant: leave ID 2 master-config bit 0 (smart power) = 0 and draw no more than the standard idle current.
- The OT/- fallback is only needed if the master must work with Lite-only boilers; a modern condensing boiler will be OT/+. Many DIY masters skip OT/-.

### Gaps
- Official Smart Power electrical/timing sequence and the exact ID 2 bit assignment are unpublished; ID 2 bit 0 = Smart Power is commonly cited by implementers but I did not find a primary source quoting the v3/v4 table.

---

## 4. Message types, conversations, mandatory IDs

### Takeaway
3-bit MSG-TYPE: master sends READ-DATA (000), WRITE-DATA (001), INVALID-DATA (010); slave replies READ-ACK (100), WRITE-ACK (101), DATA-INVALID (110), UNKNOWN-DATAID (111). Mandatory: ID 0 (read, slave must READ-ACK), ID 1 (master must write, slave must WRITE-ACK), ID 3 (master reads at least at start-up), ID 14/17/25 slave-mandatory.

### Cited Findings
- MSG-TYPE table: 000 READ-DATA, 001 WRITE-DATA, 010 INVALID-DATA, 011 reserved; 100 READ-ACK, 101 WRITE-ACK, 110 DATA-INVALID, 111 UNKNOWN-DATAID — [SPEC v2.2 §4.2.2, p.19](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- READ-DATA normally carries 0x0000 but may carry a value "e.g. for data-verification". Slave responses: READ-ACK (recognised, valid), DATA-INVALID (recognised, data unavailable/invalid), UNKNOWN-DATAID (not recognised) — [SPEC v2.2 §4.4.1, p.20](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- WRITE-DATA: slave replies WRITE-ACK / DATA-INVALID / UNKNOWN-DATAID; "Note that DATA-VALUE may be modified by the slave in some circumstances." INVALID-DATA from master: for an item that is invalid but must still be sent; slave replies DATA-INVALID or UNKNOWN-DATAID — [SPEC v2.2 §4.4.2–4.4.3, p.21](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Mandatory table: ID 0 — master "Must sent message with READ_DATA", "Must support all bits in master status"; slave "Must respond with READ_ACK", "Must support all bits in slave status". ID 1 — master "Must sent message with WRITE_DATA or INVALID_DATA (not recommended)"; slave "Must respond with WRITE_ACK". ID 3 — master "Must sent message with READ_DATA (at least at start up)"; slave READ_ACK, all config flags. ID 14 — master not mandatory, "Recommended for use in on-off control mode"; slave "Must respond with WRITE_ACK". ID 17 — slave READ_ACK or DATA_INVALID. ID 25 — slave READ_ACK or DATA_INVALID (e.g. sensor fault). "The slave can respond to all other read/write requests, if not supported, with an UNKNOWN-DATAID reply. Master units ... should be designed to act in a manner consistent with this rule." — [SPEC v2.2 §5.2, p.23](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Status exchange: master sends READ-DATA(id=0, MasterStatus, 00); slave must answer READ-ACK(id=0, MasterStatus, SlaveStatus); slave cannot answer DATA-INVALID/UNKNOWN-DATAID; "A WRITE-DATA(id=0,…) from the master should not be used." — [SPEC v2.2 §5.3.1, p.25](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- ID 0–127 are OpenTherm-defined; 128–255 are for member (manufacturer) test & diagnostic use, gated by MemberID handshake — [SPEC v2.2 §5.1, p.22](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Note in spec: v2.1+ "defines more mandatory data-ids than the versions 1.x and 2.0", so v1.x/2.0 devices may not be compatible — [SPEC v2.2 §1.3, p.7](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Reserved bits/bytes: transmitter sets 0; "the receiver should ignore these bits/bytes, since they may be set in future versions" — [SPEC v2.2 §5.1, p.23](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)

### Inferences
- Validate replies: correct parity, MSG-TYPE in the slave range (1xx), DATA-ID equal to the request's. A WRITE-ACK may echo a *modified* value (e.g. a clamped setpoint) — read back the value from the ACK rather than assuming the write took verbatim.
- Despite being "mandatory", community reports show some boilers answer ID 14 with UNKNOWN-DATAID (see §7), so the master must tolerate it.

### Gaps
- None for v2.2. v4.x may have modified the mandatory list (unpublished).

---

## 5. Data types and Data-ID reference

### Takeaway
Types: flag8, u8, s8, f8.8 (signed 8.8 fixed point, 1/256 resolution), u16, s16. Two-byte items use HB (bits 15..8) and LB (bits 7..0). Below is a dense table of the IDs relevant to a boiler master, with spec origin marked.

### Cited Findings — data types
- flag8 = 8 single-bit flags; u8 0..255; s8 −128..127; f8.8 = "signed fixed point value: 1 sign bit, 7 integer bit, 8 fractional bits (two's compliment ie. the LSB of the 16bit binary number represents 1/256th of a unit)"; u16; s16. Examples: 21.5 °C = 0x1580; −5.25 °C = 0xFAC0 — [SPEC v2.2 §5.1, p.22](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)

### Cited Findings — ID 0 Status (R, flag8/flag8) [SPEC v2.2 p.24 + v4.x bits]
- Master status (HB): bit0 CH enable; bit1 DHW enable; bit2 Cooling enable; bit3 OTC active; bit4 CH2 enable; bits5–7 reserved in v2.2 — [SPEC v2.2 §5.3.1, p.24](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- v4.x additions: bit5 "Summer/winter mode", bit6 "DHW blocking [DHW not blocking, DHW blocking]" — [OTGW-Core.ino](https://github.com/rvdbreemen/OTGW-firmware); ESPHome exposes these as `summer_mode_active` and `dhw_block` — [ESPHome OpenTherm](https://esphome.io/components/opentherm.html). **Polarity conflict:** the OTGW-firmware comment reads "[Summertime, Wintertime]" (which by the [clear,set] convention would mean set = winter), but the same code displays 'S' when the bit is set and ESPHome calls it `summer_mode_active` (set = summer). Treat set = summer as the majority interpretation, but verify.
- Slave status (LB): bit0 fault indication; bit1 CH mode (active); bit2 DHW mode; bit3 flame status; bit4 cooling status; bit5 CH2 mode; bit6 diagnostic indication; bit7 reserved in v2.2 — [SPEC v2.2 p.24](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf); v4.x bit7 = "Electricity production" — [OTGW-Core.ino](https://github.com/rvdbreemen/OTGW-firmware), ESPHome `electricity_production` — [ESPHome](https://esphome.io/components/opentherm.html)
- "The CHenable bit has priority over the Control Setpoint. The master can indicate that no CH demand is required by putting the CHenable bit = 0 (ie CH is disabled), even if the Control Setpoint is non-zero." — [SPEC v2.2 p.24](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)

### Cited Findings — Control & configuration
- **ID 1 TSet** (W, f8.8, 0..100 °C): the control setpoint = CH supply (flow) water temperature setpoint. "The master decides the actual range ... default range is assumed to be 0 to 100." Slave "only needs to control to the value", not caring whether master used room control or OTC — [SPEC v2.2 p.24](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- **ID 2 Master config / MemberID** (W, flag8/u8): all 8 flag bits reserved in v2.2; LB = master MemberID (0 = non-specific). "A valid Read Slave Configuration and Write Master Configuration message exchange is recommended before control and status information is transmitted." — [SPEC v2.2 §5.3.2, p.25](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf). v3+/v4: Smart Power support is declared here — [OTGW docs](https://otgw.tclcode.com/firmware.html)
- **ID 3 Slave config / MemberID** (R, flag8/u8): bit0 DHW present; bit1 Control type [modulating, on/off]; bit2 Cooling supported; bit3 DHW config [instantaneous or not-specified, storage tank]; bit4 "Master low-off&pump control function [allowed, not allowed]" (note: 0 = allowed); bit5 CH2 present; bits6–7 reserved in v2.2 — [SPEC v2.2 p.25](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf). v4.x: bit6 "Remote water filling function", bit7 "Heat/cool mode control" — [OTGW-Core.ino](https://github.com/rvdbreemen/OTGW-firmware); ESPHome names `water_filling`, `heat_mode` — [ESPHome](https://esphome.io/components/opentherm.html)
- **ID 4 Remote request/command** (W HB command, R LB response): v2.2: 1 = "BLOR" Boiler Lock-out Reset; 2 = "CHWF" CH water filling; response LB 0..127 = failed, 128..255 = completed. Slave may answer WRITE-ACK (accepted), DATA-INVALID (not recognised), UNKNOWN-DATAID (not supported) — [SPEC v2.2 §5.3.3, p.26](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf). v4.x adds 10 = "Service request reset" — [OTGW-Core.ino print_command](https://github.com/rvdbreemen/OTGW-firmware)
- **ID 5 ASF flags / OEM fault code** (R, flag8/u8): HB bit0 Service request; bit1 Lockout-reset [remote reset disabled, enabled]; bit2 Low water pressure; bit3 Gas/flame fault; bit4 Air pressure fault; bit5 Water over-temperature; LB = OEM fault code (meaning manufacturer-specific, for display) — [SPEC v2.2 p.24](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- **ID 6 Remote boiler parameter flags** (R, flag8/flag8): HB transfer-enable: bit0 DHW setpoint, bit1 max CH setpoint; LB read/write: bit0 DHW setpoint [read-only, read/write], bit1 max CHsetpoint. "An Unknown Data-Id response to a Read Remote-Parameter-Flags message indicates no support for remote-parameters". Up to 8 RBPs (bit n ↔ parameter n+1). Master should read bounds before writing — [SPEC v2.2 §5.3.5, p.27–28](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- **ID 7 Cooling control signal** (W, f8.8 0..100%) — [SPEC v2.2 §5.3.8.1, p.30](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- **ID 8 TsetCH2** (W, f8.8 0..100 °C), control setpoint for 2nd CH circuit — [SPEC v2.2 p.24](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- **ID 9 TrOverride** (R, f8.8, 0..30): 0 = no override, otherwise remote override room setpoint; **ID 100** (R, LB flag8) remote override function: bit0 Manual change priority (allow manual setpoint change to overrule remote setpoint), bit1 Program change priority — [SPEC v2.2 §5.3.8.3, p.30–31](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- **ID 10/11** Transparent Slave Parameters (count; index/value R/W; out-of-range index → DATA-INVALID); **ID 12/13** Fault History Buffer (size; index/value) — [SPEC v2.2 §5.3.6–5.3.7, p.29](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- **ID 14 Max relative modulation level setting** (W, f8.8 0..100%): for "sequencer and off-low&pump control applications"; "The default value in the slave device should be 100% (ie no throttling back of the capacity)." — [SPEC v2.2 §5.3.8.2, p.30](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- **ID 15** (R, u8/u8): HB max boiler capacity 0..255 kW; LB min modulation level 0..100% of max capacity — [SPEC v2.2 p.30](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- **ID 16 TrSet** (W, f8.8 −40..127) room setpoint; **ID 23** TrSetCH2 (W); **ID 24 Tr** (W) room temperature — [SPEC v2.2 §5.3.4, p.26](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- **ID 17** Relative modulation level (R, f8.8, % between min and max modulation; 0% = minimum modulation level, not off); **ID 18** CH water pressure (R, f8.8 0..5 bar); **ID 19** DHW flow rate (R, f8.8 0..16 l/min) — [SPEC v2.2 p.26](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- **ID 20** Day/time (R/W; HB bits7–5 day-of-week 1=Mon, 0=n/a; bits4–0 hours; LB minutes); **ID 21** Date (HB month, LB day); **ID 22** Year (u16) — [SPEC v2.2 p.26](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- **ID 25** Tboiler flow water temp; **26** Tdhw; **27** Toutside; **28** Tret return; **29** solar storage; **30** solar collector (s16 in §5.3.4 table, f8.8 in overview map – spec internally inconsistent); **31** TflowCH2; **32** Tdhw2; **33** Texhaust (s16, −40..500 °C). All R; temps f8.8 −40..127 unless noted — [SPEC v2.2 p.26–27, p.32](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf). OTGW-firmware marks ID 27 as R/W (master may write outside temp in later versions) — [OTGW-Core.h](https://github.com/rvdbreemen/OTGW-firmware)
- **v4.x sensor IDs** [v4.x via implementations]: 34 boiler heat-exchanger temp (f8.8); 35 boiler fan speed setpoint/actual (u8/u8; ESPHome reports RPM, OTGW labels "Hz"); 36 flame current (f8.8, µA); 37 room temperature CH2 (W, f8.8); 38 relative humidity (f8.8); 39 remote override room setpoint 2 (f8.8) — [OpenTherm.h](https://github.com/ihormelnyk/opentherm_library/blob/master/src/OpenTherm.h); [OTGW-Core.h](https://github.com/rvdbreemen/OTGW-firmware)
- **ID 48** DHW setpoint bounds (R, s8/s8: HB upper, LB lower, °C); **ID 49** max CH setpoint bounds (R, s8/s8); **ID 50** OTC heat-curve ratio bounds; **ID 56** TdhwSet DHW setpoint (R/W f8.8 0..127, remote parameter 1); **ID 57** MaxTSet max CH water setpoint (R/W f8.8 0..127, remote parameter 2); **ID 58** Hcratio OTC heat curve ratio (remote parameter 3); 51–55 / 59–63 reserved for RBP 4–8 — [SPEC v2.2 p.28, p.32](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- **70–91**: ventilation/heat-recovery IDs (not relevant for a boiler) — [OpenTherm.h](https://github.com/ihormelnyk/opentherm_library/blob/master/src/OpenTherm.h)
- **v4.x 93–99** [v4.x via implementations]: 93 Brand, 94 Brand version, 95 Brand serial number (each u8/u8: index → ASCII character, read string char by char); 96 cooling operation hours (u16); 97 power cycles of slave (u16); 98 RF sensor status (W, special: RF strength & battery); 99 remote override operating mode HC1/HC2/DHW (special) — [OpenTherm.h](https://github.com/ihormelnyk/opentherm_library/blob/master/src/OpenTherm.h); [OTGW-Core.h](https://github.com/rvdbreemen/OTGW-firmware)
- **101–108** solar storage status/config/TSP/FHB; **109–112** electricity producer starts/hours/production (W)/cumulative (kWh); **113** unsuccessful burner starts; **114** flame-signal-too-low count [v4.x via implementations] — [OpenTherm.h](https://github.com/ihormelnyk/opentherm_library/blob/master/src/OpenTherm.h)
- **ID 115** OEM diagnostic code (R, u16) — [SPEC v2.2 p.24](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- **IDs 116–123** (R/W, u16): 116 burner starts; 117 CH pump starts; 118 DHW pump/valve starts; 119 DHW burner starts; 120 burner operation hours (flame on); 121 CH pump hours; 122 DHW pump/valve hours; 123 DHW burner hours. "Reset by writing zero is optional for slave." — [SPEC v2.2 p.27](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- **ID 124** OpenTherm version master (W, f8.8); **125** OT version slave (R, f8.8); **126** master product type (HB)/version (LB) (W); **127** slave product type/version (R) — [SPEC v2.2 §5.3.2, p.25](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Manufacturer IDs ≥128 example: Remeha uses 131 (dF-/dU-codes), 132 (service message), 133 (connected SCUs) — [OTGW-Core.h](https://github.com/rvdbreemen/OTGW-firmware) [COMMUNITY]

### Inferences
- f8.8 encode: `round(t*256)` as int16; decode: `(int16_t)raw / 256.0`. Some boilers report only integer °C or 0.5 °C steps inside f8.8.
- For a modern condensing boiler, the useful read set is 0, 3, 5, 15, 17, 18, 25, 26, 27, 28, 33/34/35/36 (if supported), 48/49, 56/57, 115, 116–123, 125, 127, plus 93–95 for brand info if v4.

### Gaps
- v4.x exact definitions of ID 98/99 bit fields and of ID 2 master config bits were not found in a primary source.
- Whether 34–39 etc. are "v3.0" or "v4.x" additions could not be pinned down precisely without the official spec.

---

## 6. Protocol versions and spec availability

### Takeaway
v2.2 (≈2000–2003) is the last version freely circulating. v3.0 (approved 16 June 2008) added Smart Power and gateway/sequencer support; v4.x added many IDs (summer/winter, DHW blocking, brand strings, electricity production, ventilation, solar storage, etc.). Specs ≥3.0 are only available to OpenTherm Association members under licence.

### Cited Findings
- "All future revisions of the OpenTherm Protocol Specification must be approved by the Members of The OpenTherm Association." "The OpenTherm logo, trademark and the protocol can only be used with the permission of The OpenTherm Association. The OpenTherm Association is responsible for compliance testing procedures and licensing." — [SPEC v2.2 §2.2–2.3, p.9](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- v3.0 approved 16 June 2008 with Smart Power and multi-device connection; certification requires being "an OpenTherm member, and the product must be tested by an independent testing body"; 53 members as of 2016 — [Wikipedia: OpenTherm](https://en.wikipedia.org/wiki/OpenTherm)
- Forum claim that OT specs ≥3.0 "is not published" — [domoticaforum Smart Power thread (via search)](https://www.domoticaforum.eu/viewtopic.php?p=92666) [COMMUNITY, unverified]
- Danfoss application note references OpenTherm software v4.01; Salus documents boilers "compatible with the OpenTherm OT+ 4.0 protocol" — search results: [Danfoss AN](https://assets.danfoss.com/documents/356374/AN427239884970en-000101.pdf), [Salus manual](https://www.manualslib.com/manual/2138031/Salus-Wq610.html?page=6)
- The OTGW-firmware code cites "OTv4.2 spec §5.3" for ID 38 being f8.8 — [OTGW-Core.h](https://github.com/rvdbreemen/OTGW-firmware) (indicates some implementers have access to v4.2)
- Official site: http://www.opentherm.eu/ — [Wikipedia: OpenTherm](https://en.wikipedia.org/wiki/OpenTherm)

### Inferences
- A hobby master is free to implement from v2.2 plus community v4 knowledge, but cannot use the OpenTherm logo/claim compliance. Declaring ID 124 = 2.2 (or whatever is honestly implemented) is reasonable.

### Gaps
- No public copy of v3.0 / v4.x spec was found; no authoritative change list.

---

## 7. Practical community findings: polling, support detection, pitfalls

### Takeaway
Poll in a constant loop at ~1 message/s, always including ID 0 (status) and ID 1 (TSet) frequently; probe optional IDs and stop polling those that return UNKNOWN-DATAID (OTGW uses "3 consecutive"). Avoid re-writing configuration-like IDs (14, 56, 57) every cycle — write on change. Use CH-enable=0 to stop heating, not TSet=0.

### Cited Findings
- Spec recommendation: read ID 3 / write ID 2 before control traffic; ID 3 must be read "at least at start up" — [SPEC v2.2 §5.2, §5.3.2](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Support detection (OTGW): "After three consecutive Unknown-DataId responses for a specific Data ID" the gateway treats it as unsupported; UI/KI commands mark/reset; freed slots used for alternative requests — [OTGW firmware docs](https://otgw.tclcode.com/firmware.html) [COMMUNITY]
- ESPHome: status sensors refresh each cycle; configuration sensors read only at initialization since they "are unlikely to change without restarting the boiler"; OpenTherm "transmits its messages in a constant loop", so dropped frames are tolerated — [ESPHome OpenTherm](https://esphome.io/components/opentherm.html) [COMMUNITY]
- ESPHome pitfall: "you **must** use the `ch_enable` switch to turn off your central heating" when the setpoint number's minimum is above 0; CH enable is only sent when setpoint non-zero — [ESPHome OpenTherm](https://esphome.io/components/opentherm.html) [COMMUNITY]
- "Some boilers require certain OpenTherm messages to be sent by thermostat on initialization in order to work correctly" (hence ESPHome's options to send ID 2 controller config/ID, ID 124 version, ID 126 product type/version); Daikin D2C uses ID 162 instead of 56 for DHW setpoint — [ESPHome OpenTherm](https://esphome.io/components/opentherm.html) [COMMUNITY]
- ihormelnyk example loop sends ID 0 (setBoilerStatus, as READ-DATA with master flags in HB) then ID 1 (WRITE TSet) then reads ID 25, with `delay(1000)` per loop — [ihormelnyk README](https://github.com/ihormelnyk/opentherm_library); [OpenTherm.cpp buildSetBoilerStatusRequest](https://github.com/ihormelnyk/opentherm_library/blob/master/src/OpenTherm.cpp) [COMMUNITY]
- ID 14 issues: a boiler answered ID 14 with UNKNOWN-DATAID (spec says slave must WRITE-ACK); a Tado user reported that sending ID 14 every 6 s stopped the boiler modulating, fixed by forwarding only changes; another user had a boiler stuck at low flame until ID 14 = 100% was written — search summaries of [domoticaforum p=98807](https://www.domoticaforum.eu/viewtopic.php?p=98807), [tado community](https://community.tado.com/en-gb/discussion/19468/adding-new-id-to-opentherm-communication), [Unipi forum](https://forum.unipi.technology/uid/481) [COMMUNITY, individual reports]
- DHW setpoint (ID 56) support varies by boiler; check reported min/max (ID 48) first; a boiler may answer a write with a different value — [Home Assistant opentherm_gw set_hot_water_setpoint](https://www.home-assistant.io/actions/opentherm_gw.set_hot_water_setpoint/), [HomeSeer OTGW plugin guide](https://plugins.homeseer.com/guides/523_OpenTherm%20Gateway%20Plugin%20for%20HomeSeer%20HS4%20manual.pdf) [COMMUNITY]
- SW/SH (ID 56/57 overrides) are "only effective with boilers that support this function"; OTGW probes ID 6 and simulates reads if unsupported — [OTGW firmware docs](https://otgw.tclcode.com/firmware.html) [COMMUNITY]
- Boiler cycling at low load: users report cycling when the boiler cannot modulate low enough; lowering the boiler's minimum flow temperature reduced cycling (Tado user); Ideal + Honeywell T6 case where boiler only modulated down to ~52 °C — [tado community "Boiler cycling"](https://community.tado.com/en-gb/discussion/17395/boiler-cycling), [PlumbersForums](https://www.plumbersforums.net/threads/ideal-vogue-opentherm-via-honeywell-t6-lyric-wired-version.94115/) [COMMUNITY]
- OTGW warns: manipulating setpoints "may severely impact the control algorithm of the thermostat"; Remeha thermostat models (Celcia 20, iSense) require special handling — [OTGW firmware docs](https://otgw.tclcode.com/firmware.html) [COMMUNITY]

### Inferences
- Recommended loop for this project (synthesis): start-up: READ 3 → WRITE 2 (flags 0, MemberID 0) → WRITE 124 (2.2) → READ 125/127 → READ 6, 48, 49, 15 → probe optional IDs once. Steady state (~1 msg/s, round-robin): every other slot ID 0 (status with CH/DHW enable) and ID 1 (TSet); remaining slots rotate 25, 17, 28, 18, 5 (only when fault bit set), 27, 26, 33/35/36 (if supported), counters 116–123 rarely (e.g. hourly). Drop IDs after 3 consecutive UNKNOWN-DATAID; retry them occasionally (e.g. after reconnect).
- Writes to 56/57 and 14: only on change (and maybe once after reconnect). EEPROM wear is a plausible but **unverified** concern — no source found confirming that boilers store ID 56/57 writes in EEPROM; write-on-change avoids the issue either way.
- To stop heating: clear CH enable bit in ID 0 rather than sending TSet = 0; keep ID 1 flowing with a valid value (spec gives TSet range 0..100; many boilers clamp to their own min/max e.g. via ID 49/57).
- For modulation-friendly control of a condensing boiler, keep TSet low (low return temperature) and avoid burner cycling by respecting the boiler's minimum modulation (ID 15 LB) — the boiler's own anti-cycling logic still applies.

### Gaps
- No systematic dataset of which IDs common boilers (Vaillant, Viessmann, Bosch, Baxi, Ariston, Remeha, Intergas) support was found; it varies per model and must be probed.
- No source confirms or refutes EEPROM wear from writing 56/57.

---

## 8. Safety: boiler vs master responsibilities

### Takeaway
The boiler (slave) remains responsible for all appliance safety: flame supervision, over-temperature, pressure, lock-out, and controlling to the setpoint in its own way. The master only requests a flow temperature and enables; it must not assume its commands bypass boiler limits. The master's key safety obligations are to keep communicating (to avoid the short-circuit heat-demand fallback) and to cap the setpoint sensibly.

### Cited Findings
- Division of roles: the room controller calculates a heating demand as a water-temperature Control Setpoint; "The slave does not need to know how the master has calculated the control setpoint ... it only needs to control to the value. Likewise, the master does not need to know how the slave is controlling the supply." — [SPEC v2.2 §2.1, §5.3.1, p.8, p.24](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Fault reporting comes from the slave (ID 0 fault bit, ID 5 ASF flags incl. low water pressure, gas/flame fault, air pressure, water over-temperature) — the master only reads/displays them; remote lock-out reset (ID 4 cmd 1) is only possible if the boiler allows it (ID 5 HB bit1 "Lockout-reset [remote reset disabled, rr enabled]") — [SPEC v2.2 p.24–26](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Remote boiler parameters have bounds the master "should read from the slave in order to make sure they are not set outside the valid range" — [SPEC v2.2 §5.3.5, p.27](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Boiler interface provides mains isolation (EN60730-1) — [SPEC v2.2 §3.3.2.4, p.15](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- Short-circuit / idle-line = heat demand (see §2) — [SPEC v2.2 §3.5, p.17](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)
- OTGW treats repeated refresh of an external setpoint as "a vigilance check to prevent runaway heating" — [OTGW firmware docs](https://otgw.tclcode.com/firmware.html) [COMMUNITY]

### Inferences
- Master-side safety measures worth implementing: hardware watchdog; software cap on TSet (e.g. ≤ boiler max via ID 57/49 and an application cap such as 55–60 °C for underfloor/radiators as appropriate); room-temperature sensor sanity checks (stale/implausible reading → safe mode with CH disabled or a conservative setpoint); never blindly auto-reset lock-outs (ID 4 BLOR) in a loop.
- Note the inversion of failure modes: a master that dies with the line idle may cause *maximum* heating (short-circuit rule), not "off". Design the failure state deliberately.

### Gaps
- Exact boiler-internal limits (max flow temp, anti-cycling timers, frost protection) are boiler-specific and not part of the protocol.
