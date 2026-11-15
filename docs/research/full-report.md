# Drive the Victrix Tera with OpenTherm, safely

The DIY ESP32-P4 thermostat can almost certainly run the Immergas Victrix Tera 24 Plus as a modulating OpenTherm master, but only if three conditions hold. First, the boiler is the **V2** generation with installer parameter **P7 = 2 ("comando remoto commerciale")**. Second, the firmware adds the Immergas quirks: status low byte **0xCA**, ID 57 mirrored to ID 1, and ID 1 = 10 °C whenever CH is off. Third, the design accepts that a dead master leaves this boiler **cold, not runaway**. On loss of the bus the Tera raises **E31**, and after a power cycle it refuses central heating until a remote or the X40 jumper returns. For Phase 1 the evidence supports a **load-compensating PI controller**: room error from the single SHT40 sets the flow setpoint (ID 1). It should be bounded to roughly 30–55 °C and sit on a conservative fixed base, or on a weather curve once a reliable outdoor temperature exists. Hard anti-cycling rules matter as much. The boiler's 4.3 kW minimum output (about 18 % of CH nominal, a 5.6:1 turndown) will exceed the house's heat loss on mild days, and short burns cost 1.5–12 % efficiency. That loss is as large as the whole modelled benefit of modulation at radiator temperatures. The regulatory gain from modulation plus weather compensation is a modest **+3–4 percentage points** (ErP Classes V/VI). Field evidence for anything beyond a few percent is thin. The real wins are low return temperatures (below about 55 °C), long burns and a slightly cooler, better-held room, not machine learning. Machine learning becomes worthwhile later as grey-box system identification and optimum start, and it needs a full logging schema and a gas-meter baseline from the first day the boiler is connected.

This report runs from the bus up. It covers the protocol, the specific boiler and its Immergas dialect, the physics and control strategies, DHW and hydraulics, reference implementations, learning and KPIs, and safety. It ends with a concrete Phase 1 design, a roadmap, a bench-test plan for the real boiler and an installer checklist. Each top-level section is written to stand alone as a study chapter.

---

## 1. OpenTherm is a 1 kbit/s current-loop dialogue that the master must never let lapse

### 1.1 Physical layer: voltage one way, current the other

OpenTherm is a **two-wire, polarity-free, point-to-point** link, at most 50 m long with no more than 2 × 5 Ω of cable resistance. The boiler (slave) powers the line. The master signals by switching line **voltage**: idle/low ≤ 7 V, high 15–18 V, with the boiler's receive threshold at 9.5–12.5 V. The boiler answers by switching loop **current**: idle/low 5–9 mA, high 17–23 mA, with the master's threshold at 11.5–14.5 mA. Rise and fall times are at most 50 µs ([OpenTherm Protocol v2.2 §3.1–3.2, pp.12–14](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)). Because the boiler guarantees only the minimum idle current, a line-powered room unit may draw **at most 5 mA**, about 35 mW ([spec §3.3](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf); [Wikipedia](https://en.wikipedia.org/wiki/OpenTherm)). An ESP32-P4 with a display and camera therefore has to be locally powered, which the spec explicitly allows. The DIYLESS Master shield is built that way: it is powered from the MCU's 3.3 V/5 V rail, and its boiler terminals are "polarity does not matter" ([DIYLESS Master shield (mirror)](https://diyless-old.duckdns.org/product/master-opentherm-shield); [DIYLESS thermostat shield (mirror)](https://diyless-old.duckdns.org/product/esp8266-thermostat-shield)).

Bits use **Manchester (bi-phase-L)** coding at a nominal 1000 bit/s. A '1' is an active-to-idle transition and a '0' is idle-to-active. The spacing between mid-bit transitions may range from 900 to 1150 µs, and the receiver must "reset timing on each transition" so that errors do not accumulate ([spec §3.4](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)). A frame is a start bit '1', 32 data bits and a stop bit '1', so it takes **34 ms** on the wire. The 32 bits are, MSB first: `P(1) | MSG-TYPE(3) | SPARE(4) | DATA-ID(8) | DATA-VALUE(16)`. Parity is set so that the total number of 1 bits is even, and the spare bits are 0 ([spec §4.2](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)). Any Manchester or parity error rejects the whole frame. "No errors are treated as recoverable"; the master simply retries at the next scheduled slot ([spec §4.5](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)).

On the MCU side, the widely used Ihor Melnyk library bit-bangs 500 µs half-bits with `delayMicroseconds`, decodes on a pin-change ISR with a 750 µs threshold, declares TIMEOUT after 1 s and waits 100 ms after each response ([OpenTherm.cpp](https://github.com/ihormelnyk/opentherm_library/blob/master/src/OpenTherm.cpp)). ESPHome warns that other drivers which disable interrupts (Dallas 1-wire is the classic case) cause intermittent invalid frames. Its `sync_mode` "usually won't eliminate them entirely" ([ESPHome OpenTherm](https://esphome.io/components/opentherm/)). On a P4 that is also running Wi-Fi via the C6, a display, touch and a camera, this argues for hardware-timed transmit (RMT or a GPTimer) and edge capture on a dedicated, high-priority task.

### 1.2 Timing: one conversation at a time, at least once a second

The master sends one request. The slave must answer **20–800 ms after the end of the request**. The master then waits **≥100 ms** before the next request, and it "must communicate at least every 1 sec (+15 % tolerance)", which gives a hard ceiling of 1.15 s ([spec §4.3.1, p.19](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)). In practice a conversation takes 250–400 ms. OTGW observes that "the boiler generally answers within 200 ms" and defaults to one message per second in stand-alone mode ([OTGW firmware docs](https://otgw.tclcode.com/firmware.html)). Immergas boilers reportedly answer "in a few tens of ms" ([jotuera hub docs](https://raw.githubusercontent.com/jotuera/immergas-modbus-bms-nasa-opentherm-hub/main/docs/opentherm-hub.md)), so two or three conversations per second are feasible. One DIYLESS user on an Immergas Victrix Extra 12 Plus nevertheless lost **17.6 %** of replies, which DIYLESS attributed to answers near or beyond the 800 ms window ([diyless thermostat3 #32](https://github.com/diyless/opentherm-thermostat3/issues/32#issuecomment-5373052226)). The firmware must treat a missed reply as routine.

### 1.3 OT/+, OT/- and Smart Power

OT/+ is the digital Manchester protocol, and it is the only level that may carry the logo. OT/- ("Lite") shares the physical layer but sends a **100–500 Hz PWM** whose duty cycle maps linearly (10–90 %) onto the setpoint, with **<5 % meaning CH disabled** ([spec §6, pp.34–35](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)). On power-up a master tries OT/+ for 20 s before falling back to OT/-. Once in OT/+, it enters a "communications fault" state after **1 minute** without a valid response ([spec §2.3](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)). Smart Power arrived in v3.0 (approved 16 June 2008). It lets the master request medium (136 mW) or high (255 mW) line power ([Wikipedia](https://en.wikipedia.org/wiki/OpenTherm)), but its signalling is unpublished and known only by reverse engineering ([OTGW docs](https://otgw.tclcode.com/firmware.html)). Neither OT/- nor Smart Power matters to a locally powered master driving a modern condensing boiler. Leave ID 2 flags at 0.

### 1.4 Message types and the mandatory core

The master sends READ-DATA (000), WRITE-DATA (001) or INVALID-DATA (010). The slave answers READ-ACK (100), WRITE-ACK (101), DATA-INVALID (110) or UNKNOWN-DATAID (111). A WRITE-ACK may carry a **modified** value, such as a clamped setpoint, so the master should read back what was actually accepted ([spec §4.4](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)). The v2.2 mandatory set is short. ID 0 (status) must be read, ID 1 (control setpoint) must be written, and ID 3 (slave config) must be read "at least at start up". The slave must also answer ID 14, 17 and 25. Every other ID may legitimately return UNKNOWN-DATAID, and masters "should be designed to act in a manner consistent with this rule" ([spec §5.2, p.23](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)). IDs 0–127 are standard. IDs 128–255 are manufacturer test and diagnostic IDs ([spec §5.1](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)). Two rules follow for the master. A reply is valid only if parity is correct, the type is in the slave range and the Data-ID echoes the request. "Unsupported" must never be confused with "link down".

### 1.5 The Data-IDs that matter for a boiler master

Data types are flag8, u8, s8, u16, s16 and **f8.8**, a signed fixed-point value with 1/256 resolution, so 21.5 °C = 0x1580 ([spec §5.1](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)). The table condenses the v2.2 spec and the v4.x additions known from implementations ([spec pp.24–32](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf); [ihormelnyk OpenTherm.h](https://github.com/ihormelnyk/opentherm_library/blob/master/src/OpenTherm.h); [OTGW-firmware](https://github.com/rvdbreemen/OTGW-firmware); [ESPHome](https://esphome.io/components/opentherm.html)).

| ID | Name | Dir/type | Notes for this project |
|---|---|---|---|
| 0 | Status | R, flag8/flag8 | Master HB: b0 CH enable, b1 DHW enable, b2 cooling, b3 OTC active, b4 CH2; v4 b5 summer/winter (polarity disputed), b6 DHW blocking. Slave LB: b0 fault, b1 CH active, b2 DHW active, b3 **flame**, b5 CH2, b6 diagnostic. "CHenable has priority over the Control Setpoint". **Immergas: request LB = 0xCA** |
| 1 | TSet | W, f8.8 0–100 °C | CH flow setpoint, the main control lever |
| 2 | Master config/MemberID | W | Flags 0, MemberID 0 |
| 3 | Slave config | R | b0 DHW present, b1 on/off vs modulating, b3 DHW storage tank, b4 low-off & pump control (0 = allowed), b5 CH2 present |
| 4 | Remote command | W | 1 = boiler lock-out reset, only if ID 5 HB b1 allows it; never in a loop |
| 5 | ASF flags/OEM fault | R | Service, low water pressure, flame fault, air pressure, over-temperature; LB OEM code |
| 6 | Remote-parameter flags | R | Whether 56/57 are transferable and writable |
| 8 | TsetCH2 | W | Second CH circuit; rarely implemented |
| 14 | Max rel. modulation | W, % | Default 100 %. Some boilers ignore it or misbehave if spammed. Write on change |
| 15 | Max capacity kW / min modulation % | R | Anchors low-load logic |
| 16 / 24 | Room setpoint / room temp | W | Informational for most boilers |
| 17 | Relative modulation | R, % | 0 % = minimum modulation, not off |
| 18 | CH pressure | R, bar | |
| 19 | DHW flow | R | Absent on tank/system boilers |
| 25 / 28 | Flow / return temp | R | Return is the condensing KPI |
| 26 / 27 / 33 | Tank or DHW temp / outside / exhaust | R | Model-dependent |
| 34–36 (v4) | Heat-exchanger temp, fan speed, flame current | R | If supported |
| 48 / 49 | DHW / max-CH setpoint bounds | R, s8/s8 | Read before writing 56/57 |
| 56 / 57 | DHW setpoint / max CH setpoint | R/W | May be panel-only on the Tera with P7 = 2 |
| 93–95 (v4) | Brand / version / serial strings | R | |
| 115 | OEM diagnostic code | R | |
| 116–123 | Starts and hours counters | R/W | Burner starts, pump starts, burner hours |
| 124–127 | OT version and product versions | W/R | Declare honestly (e.g. 2.2) |

### 1.6 Versions: v2.2 is public, v3/v4 are members-only

The 2003-era **v2.2** is the last freely circulating specification. v3.0 (2008) added Smart Power and multi-device support, and v4.x added many IDs, among them summer/winter mode, DHW blocking, brand strings, solar storage and ventilation. Specs from v3.0 onward are licensed to Association members, and certification needs membership plus independent testing ([Wikipedia](https://en.wikipedia.org/wiki/OpenTherm); [spec §2.2–2.3](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)). Community firmware cites "OTv4.2 spec §5.3", which shows that some implementers have seen it ([OTGW-Core.h](https://github.com/rvdbreemen/OTGW-firmware)). A hobby master can implement v2.2 plus community v4 knowledge, but it cannot claim compliance. The v4 summer/winter bit is a case to be careful with: OTGW's comment and ESPHome's naming disagree on its polarity.

### 1.7 Comm loss and the short-circuit rule invert the intuitive failure mode

The spec requires every boiler to treat **shorted terminals as a heat demand** within 15 s. That is an installer test feature, and it may be detected in software as "Vlow with no valid frame for at least 5 s" ([spec §3.5, p.17](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)). Electrically, an idle line looks the same as a short. **A spec-compliant boiler whose master hangs with the line idle may therefore start heating.** OTGW raises the line to Vhigh when the thermostat stops sending for exactly this reason ([domoticaforum, via search snippet](https://www.domoticaforum.eu/viewtopic.php?p=100938)). It also applies a "vigilance check to prevent runaway heating": an external control setpoint must be refreshed every minute or it is dropped ([OTGW docs](https://otgw.tclcode.com/firmware.html)). Beyond the short-circuit rule, the spec does **not** define what an OT/+ boiler does on comm loss. Behaviour is manufacturer-specific: Baxi shows E87, Nest H73, Remeha iBase faults after 60 s, and one boiler ran a fixed 60 °C ([tado/Baxi](https://community.tado.com/en-gb/discussion/21391/baxi-error-e87); [Remeha iBase manual](https://www.manualslib.de/manual/526801/Remeha-Ibase.html?page=25)). As Section 3 shows, the Immergas Tera goes the opposite way from "runaway". Both failure directions must be designed for and then tested on the real boiler.

---

## 2. The Victrix Tera 24 Plus is a 4.3–24 kW system boiler whose own settings bound everything the master does

### 2.1 Two generations with the same name

"Plus" is the heating-only (system) variant. It has an internal motorised 3-way valve and is designed to feed a separate Immergas tank unit of 80, 120 or 200 L. There are **two generations**: the original Victrix Tera 24 Plus (manual STD.008065/002) and the **Victrix Tera V2 24 Plus EU** (code 3.032933, manual STD.009317/003). They differ on the one parameter that decides OpenTherm support, P7 ([V1 manual](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf); [V2 manual](https://gimli.freetls.fastly.net/tavolla/Products/Attachments/40503/inst.pdf)). The Hungarian-language manuals on immergas.hu for the 24 Plus are still the **original version, with P7 "Nem elérhető ennél a modellnél"** (not available on this model). The HU V2 *combi* manual has P7 = 2 for a commercial remote ([immergas.hu Plus HU](https://immergas.hu/sites/default/files/termekek/kazan/dokumentumok/Victrix-Tera%2024%2035%20Plus_HU.pdf); [immergas.hu V2 HU](https://immergas.hu/sites/default/files/documents/Victrix%20Tera%20V2%2028-32-38_HUN.pdf)). For a 2026 purchase in Hungary, the order, invoice and rating plate should all say **V2**.

### 2.2 Output, modulation and efficiency

The V1 data gives **24.1 kW useful CH output, 28.3 kW to the tank and 4.3 kW minimum**. NCV efficiency is 97.8 % at 80/60, 106.1 % at 50/30 and 108.2–108.3 % at 40/30. ηs is 93 %, energy class A ([V1 manual §4.1–4.6](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)). V2 is nearly identical: 24.0/28.0/4.3 kW, 108.0–108.4 % at 40/30, ηs 94 %, and a CH range of 20–85 °C ([V2 manual §4.3](https://gimli.freetls.fastly.net/tavolla/Products/Attachments/40503/inst.pdf)). The CH turndown is therefore **about 5.6:1**, and the minimum output is about 18 % of CH nominal. The roughly 10-point efficiency gap between 80/60 and 40/30 is the condensing prize the control strategy chases. In the V1 output table, CH maximum sits at **84 % "modulation"** on G20. The relative modulation reported over OpenTherm (ID 17) may therefore top out near the P2 limit rather than 100 %, and one Immergas hub notes ID 17 "can exceed 100 % during DHW" ([jotuera hub](https://raw.githubusercontent.com/jotuera/immergas-modbus-bms-nasa-opentherm-hub/main/docs/opentherm-hub.md)). Both points need verifying.

### 2.3 Hydraulics inside the casing

The boiler contains a **PWM variable-speed pump** (Grundfos UPM3 or UPM4) with three modes: proportional head (A3 = 0), **constant ΔT (A3 = 5–25 K, default 15 K)** or fixed speed. In DHW mode the pump runs at maximum. There is also an **adjustable internal bypass that leaves the factory open**, a stepper 3-way valve, a 5.8 L expansion vessel and a 3 bar safety valve ([V1 manual §1.29–1.30, §3.4](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)). The tank connects through an **NTC probe (B2) on terminals 36–37**, with resistor R8 removed. Error E12 (tank-sensor fault) and info item d0.7 (tank temperature) show that the boiler reads a sensor, not a thermostat contact ([V1 manual pp.16, 45–47, 52–53](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)).

### 2.4 Installer parameters a third-party master must respect

The installer menu is opened by holding two buttons for more than 5 s and entering an unpublished password ([V1 manual §3.11](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf); [V2 manual pp.80–82](https://gimli.freetls.fastly.net/tavolla/Products/Attachments/40503/inst.pdf)). The parameters that shape OpenTherm control are these:

| Parameter | Meaning | Range / default | Why it matters |
|---|---|---|---|
| **P7** (V2 only) | Remote protocol: 0 IMG BUS, 2 commercial remote on 44/41 | default 0 | Must be 2 for a third-party master |
| P1 / P2 | Min / max CH power | P2 default 83–85 % | P2 caps CH output and may define ID 17's ceiling |
| P4 | Pump: 0 intermittent (follows demand), 1 continuous in winter | 0 | Post-circulation time undocumented |
| P5 | Outdoor-sensor correction | ±9 K | |
| t0 / t1 | Min / max CH setpoint | 25 / 85 °C | Boiler-side clamp on ID 1 |
| t3 | DHW priority hold after tank charge | 0–100 s step 10, default "2" (probably 20 s) | |
| **t4** | CH re-ignition (anti-cycle) timer | 0–600 s step 10, default "18" (probably **180 s**) | Minimum off-time the master cannot beat |
| t5 | CH ramp to max power | default "18" (probably 180 s) | Soft start |
| t6 | Delay after a call (for zone valves) | 0–600 s, default 0 | |
| A0 | Hydraulics (1 = tank) | 1 | |
| A3 | Pump ΔT mode | default 15 K | Large ΔT lowers return temperature |
| A4 / A5 | Tank flow offset / tank call hysteresis | 25 K / 3 K | Tank charge flow = DHW set + A4 |
| S2 | Ignition fan speed | 44 (×50 rpm) | Drives the high-fire burst at ignition |

The boiler protects itself. Frost protection starts the pump and burner below 4 °C water temperature, and a radiator anti-freeze function heats to 42 °C if the return falls below 4 °C. Both remain active in stand-by but not in "off". The pump and 3-way valve anti-seize every 24 h ([V1 manual §1.4, §3.18–3.20](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)). The **outdoor sensor** (B4, terminals 38–39, curve 0–9) "always acts when connected, whatever room control is used" ([V1 manual §1.11, §2.5](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)). How it interacts with an OpenTherm ID 1 setpoint is undocumented. If the thermostat does its own compensation, the cleaner choice is not to fit a boiler-side sensor, or to fit one and verify the interaction. The screed-drying function requires that **no remote is connected**. Useful info-menu items for cross-checking what the master reads include d0.2 flow, d0.4 CH set, d0.8 return, d1.4 pump flow, d1.5 fan rpm, d1.6 flue temperature, d1.7 calculated setpoint and d1.9 software versions.

---

## 3. Immergas speaks "OpenTherm, modified", and the 0xCA byte, P7 and E31 define the integration

### 3.1 IMG BUS is OpenTherm-like, and P7 = 2 is the official door

Immergas never uses the word "OpenTherm" for the Tera, and it is reportedly not an OpenTherm Association member. Its remote bus is **IMG BUS**. A technician called it "almost OT, but improved a bit", and Immergas support told one user the Tera 28 supports "Open Therm modificato" ([HA community thread](https://community.home-assistant.io/t/immergas-victrix-integration-with-ha/395191); [plcforum Nest thread](https://www.plcforum.it/f/topic/304253-immergas-vitrix-tera-nest-heat-link/)). The decisive official text is in the V2 manual. **P7 "Comando remoto"**: 0 = IMG BUS for Immergas remotes on 44/41; **2 = "comando remoto commerciale ai morsetti 44/41"**. With P7 = 2, "the DHW and max CH flow settings remain available on the boiler panel." On V1, P7 is "non disponibile su questo modello" ([V2 manual p.80](https://gimli.freetls.fastly.net/tavolla/Products/Attachments/40503/inst.pdf); [V1 manual p.56](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)). The remote connects to terminals **41 and 44/40**, after removing jumper **X40**. The manual says "respecting polarity", and only one remote may be connected ([V1 manual p.17](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)). The same terminals take a volt-free on/off contact, which must be "pulito" (clean) or the board is damaged. There is no 0–10 V input and no Immergas OpenTherm kit for this boiler ([V2 brochure](https://www.immergas.com/media/Prodotto/63d279c7bc8e3c6a1f42b67e/VICTRIX%20TERA%20V2)).

An open-circuit voltmeter reading distinguishes the buses. **Up to about 18 V DC is IMG BUS**, which works with OpenTherm. **About 24–35 V DC is the older CAR-BUS**, "which no OpenTherm device can talk to"; a Victrix TT measured 28 V ([HA thread p.2](https://community.home-assistant.io/t/immergas-victrix-integration-with-ha/395191?page=2); [OTGateway #284](https://github.com/Laxilef/OTGateway/issues/284)).

### 3.2 The quirks: 0xCA, ID 57 mirroring and an explicit "off" setpoint

OTGateway's "Fix for Immergas boilers" changes exactly **one byte**. ID 0 is sent as READ-DATA with the **low byte = 0xCA** instead of 0x00, so the frame data is 0x03CA with CH and DHW enabled ([OTGateway OpenThermTask.h](https://github.com/Laxilef/OTGateway/blob/master/src/OpenThermTask.h); [Laxilef library fork](https://github.com/Laxilef/opentherm_library/blob/esp32_timer/src/OpenTherm.cpp)). The discoverer, on an Immergas Eolo Star, worked out the sequence experimentally: ID 0 = 0x03CA, then read ID 48/49, then write ID 1 and ID 56, then write ID 57, with the heating temperature written **to both ID 1 and ID 57**. "Without the first step, writes to these registers change nothing" ([arduino.ru p.15](https://arduino.ru/forum/programmirovanie/termostat-opentherm-na-esp8266?page=15)). OTGateway's ID 57 mirroring ("Sync max heating temp with target temp") is a separate option, and the jotuera hub notes that "some Immergas boilers (EOLO Star, Mini EOLO) acknowledge Data-ID 1 and heat to Data-ID 57." The same hub sends **ID 1 = 10 °C when CH is off**, because "some boilers keep firing to the last flow setpoint with only the CH bit cleared" ([jotuera hub](https://raw.githubusercontent.com/jotuera/immergas-modbus-bms-nasa-opentherm-hub/main/docs/opentherm-hub.md)). A Fantini OT chrono on a Tera TT 32 did exactly that: it kept heating with the chrono off ([plcforum](https://www.plcforum.it/f/topic/304469-victrix-tera-24-netatmo-modulante-opentherm/)). SAT and OTGW-firmware go further and write Immergas transparent slave parameter **TSP 12** with min(max-modulation, 80). Its meaning is undocumented, it was never tested on a real boiler, and its MemberID 27 is shared with Sime ([SAT opentherm.py](https://github.com/Alexwijn/SAT/blob/main/custom_components/sat/mqtt/opentherm.py); [OTGW-firmware CHANGELOG](https://github.com/rvdbreemen/OTGW-firmware/blob/dev/CHANGELOG.md)). Read it, but do not write it.

### 3.3 Field reports: more successes than failures, none of which state V1 or V2

| Boiler | Master / adapter | Result | Source |
|---|---|---|---|
| **Victrix Tera 24 Plus** | OTGateway + DIYLESS ESP8266 shield, Immergas fix | **Works** (Jan 2026) | [OTGateway #3](https://github.com/Laxilef/OTGateway/issues/3#issuecomment-3801355953) |
| **Victrix Tera 24 Plus** ("original, ~2021") | ESP32 + DIYLESS shield, OTGateway and ESPHome | **All TIMEOUT**, no reply frame; bus voltage, GPIO swap and P7 never checked; stale | [OTGateway #284](https://github.com/Laxilef/OTGateway/issues/284) |
| Victrix Tera 28 | DIYLESS on 40/41, fix | Works, except DHW flow | [OTGateway #3](https://github.com/Laxilef/OTGateway/issues/3#issuecomment-3369153481) |
| Victrix Exa 24 X 1 ErP | DIYLESS Master shield, fix | Works; pressure and exhaust not reported | [OTGateway #3](https://github.com/Laxilef/OTGateway/issues/3#issuecomment-3747914846) |
| Victrix Zeus 25 | DIYLESS Master shield, fix | Works; ID 19 invalid ("only in models … without a storage tank") | [OTGateway #3](https://github.com/Laxilef/OTGateway/issues/3#issuecomment-2453361792); [#92](https://github.com/Laxilef/OTGateway/issues/92) |
| Victrix Zeus 25 / Omnia | DIYLESS Thermostat 3 | Works but **short-cycles**: ignites about 38 % for about 20 s, cuts out at setpoint +5 K, about 3 min wait | [diyless #34](https://github.com/diyless/opentherm-thermostat3/issues/34); [#31](https://github.com/diyless/opentherm-thermostat3/issues/31#issuecomment-3744691452) |
| Victrix Tera 24 | Nest Heat Link on 40-41 | Works "alla perfezione", flow about 45 °C; only DHW max settable; errors not shown | [plcforum](https://www.plcforum.it/f/topic/304253-immergas-vitrix-tera-nest-heat-link/) |
| Victrix Tera 24 **V2** | tado | Works after **P7 = 2** (one report; a second user could not confirm) | [fibra.click (snippet)](https://forum.fibra.click/d/62405-termostato-opentherm/13) |
| Victrix 32 ErP | evohome R8810 | DHW stuck at 30 °C, CH at 85 °C | [HA thread](https://community.home-assistant.io/t/immergas-victrix-integration-with-ha/395191) |

The pattern is that the Tera family speaks an IMG BUS that ordinary OpenTherm masters drive. The DIYLESS shield is the most-reported working hardware, and the lone Tera 24 Plus failure looks more like wiring, bus or version trouble than a protocol block. **No report confirms that a V1 unit with P7 unavailable works**, and that uncertainty is exactly why the V2 purchase matters.

### 3.4 E31: on comm loss this boiler fails cold

The manual defines **E31** as "loss of communication with the remote". It occurs with an incompatible remote or when communication drops, and the reset is a power cycle. "If at power-up the remote control is not detected, the boiler goes to local mode… in this case the 'Heating' function cannot be activated". **DHW still works; CH does not** ([V1 manual p.46](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf); [HU V2 manual](https://immergas.hu/sites/default/files/documents/Victrix%20Tera%20V2%2028-32-38_HUN.pdf)). Without a remote, the boiler "waits for the remote (E31) and doesn't heat" until X40 is replaced and the boiler power-cycled. Unlike Zeus Superior/Hercules (M3·P33 = ON), the Tera has no documented "keep heating on remote loss" option ([HA thread p.2](https://community.home-assistant.io/t/immergas-victrix-integration-with-ha/395191?page=2)). The time until E31 is undocumented. OTGateway retries 5 times with 1 s timeouts, about 6.3 s, before declaring a disconnect ([OTGateway OpenThermTask.h](https://github.com/Laxilef/OTGateway/blob/master/src/OpenThermTask.h)).

This reverses the generic OpenTherm hazard of Section 1.7. A dead master on a spec-literal boiler can mean runaway heat. A dead master on the Tera means **an unheated house in winter**, protected only by the boiler's 4 °C water frost function. Whether the Tera also honours the spec's idle-line short-circuit rule while configured with P7 = 2 is unknown, so both possibilities must be tested. The robust fallback the notes point to is a **relay contact across 44/40–41**. It is held open while a hardware watchdog confirms the firmware is alive, and it closes when the watchdog expires. That re-creates "X40 present", so the boiler heats on its own panel setpoint. Whether the boiler accepts the switch from OT to a closed contact **without a power cycle** is unverified. The manual's wording implies that it may need one.

---

## 4. Condensing physics rewards a 30–45 °C return, and load compensation is the evidence-backed Phase 1 strategy

### 4.1 Return temperature is the efficiency dial

Natural-gas flue gas has a dew point of about **55.6 °C**, so the boiler condenses only when the water returning to the heat exchanger is below roughly 55 °C. The gain steepens below that point ([Bosch whitepaper](https://www.bosch-industrial.com/africa/media/country_pool/knowledge/technical-reports/whitepaper_condensing.pdf); [BRE 2017 pp.10–11](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)). The Tera's own data puts this at 97.8 % NCV at 80/60 against 108 % at 40/30 (Section 2.2). Radiators follow the EN 442 law Q = Q₅₀·(ΔT/50)^n, where ΔT is mean water temperature minus room temperature and n ≈ 1.3 ([Trystan Lea](https://trystanlea.org.uk/heatpumpradiators); [Zehnder](https://www.zehnder.co.uk/en/decorative-radiators/resources/correction-factors-delta-t-conversion)). Measured double-panel radiators sometimes fit n ≈ 1.5 better ([OpenEnergyMonitor](https://community.openenergymonitor.org/t/rating-radiators-at-off-design-conditions/27313)). At a 20 °C room, the output fraction of the ΔT50 rating by mean water temperature is:

| Mean water temp | 70 °C | 60 °C | 55 °C | 50 °C | 45 °C | 40 °C | 35 °C |
|---|---|---|---|---|---|---|---|
| Output fraction (n = 1.3) | 1.00 | 0.75 | 0.63 | 0.51 | 0.41 | 0.30 | 0.21 |

A radiator system sized for 75/65 at design therefore needs only a mean of about 45 °C when the load is 40 % of design, which is a typical shoulder-season day. With the pump's default 15 K ΔT (A3) that is roughly 52/37, comfortably condensing. A large ΔT lowers the return at a fixed mean temperature, and an over-fast pump or open bypass destroys it. The underfloor circuit, mixed down from the boiler flow, returns cool water and helps. However, if the radiators drive the boiler setpoint, the mixer adds no efficiency of its own.

### 4.2 Ranking the strategies: on/off < TPI < load compensation ≈ weather compensation < combined

On/off thermostats swing about 1–1.5 °C (electronic) and overshoot. TPI cycles within a fixed period, for example 4 min on and 6 min off for a 40 % demand ([BRE 2017 §2.5](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)). **Load compensation** uses the indoor error to set a modulating flow temperature, and **weather compensation** uses the outdoor temperature on a heating curve. Both save energy for the same reason: a lower return at low demand ([BRE 2017 §2.3](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)). The EU ErP control classes (Reg. 811/2013) codify the ladder:

| ErP class | Control | Seasonal-efficiency credit |
|---|---|---|
| I | On/off room thermostat | 1 % |
| II | Weather compensator, modulating | 2 % |
| IV | TPI | 2 % |
| **V** | **Modulating room thermostat (load compensation)** | **3 %** |
| **VI** | **Weather compensator + room sensor, modulating** | **4 %** |
| VII | Weather compensator + room sensor, on/off | 3.5 % |
| **VIII** | **Multi-sensor (≥3 room sensors) modulating** | **5 %** |

BRE notes that the technical origin of these credits "is not stated, so their usefulness is questionable" ([BRE 2017 Table 1](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf); [Nest ErP sheet](https://nest.com/support/images/misc-nest-thermostat-eu/064-00025-GB-A_A2_ErP_English.pdf)). Modelled SAP gains over a standard thermostat climb steeply as design temperatures fall. Load or weather compensation gives **0.7 % at 80/60, 3.4 % at 55/47 and 6.1 % at 45/39**, and the combination gives 1.8 % / 5 % / 6.9 % ([BRE 2017 Table 2](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)). Measured evidence is sparse and spread out. Salford's BEAMA-commissioned tests found about 10 % for proportional load-compensating on/off and about **12 % for directly modulating load or weather compensation**, with load compensation the largest single saving ([BEAMA](https://www.beama.org.uk/news/the-heat-is-on-beama-led-research-reveals-heating-controls-can-reduce-gas-use-by-10-12-percent-amid-record-price-hikes.html); [HVP](https://www.hvpmag.co.uk/BEAMA-examines-energy-saving-potential-of-load-and-weather-comp-controls/12769)). That work is industry-sponsored. The only large field trial (TPI, about 50 homes) found no significant difference ([BRE 2017 §5.7](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)), and a BEIS review found "limited international evidence, high quality or otherwise" ([BEIS](https://assets.publishing.service.gov.uk/government/uploads/system/uploads/attachment_data/file/650840/heating-controls-international-evidence.pdf)). The honest expectation is a few percent of gas, with more if the system can actually run at 45/35-type temperatures. A 1 °C lower setpoint is worth "around 10 %" on its own ([BRE 2017 §5.1](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)), so tight control that lets occupants accept a lower setpoint can outweigh the strategy choice.

With **only an indoor sensor**, load compensation (Class V) is the right Phase 1 choice. When an outdoor reading becomes reliable, **curve plus room trim** (Class VI) is the target. Weather compensation alone is open-loop and must be tuned. It recovers slowly from intermittent schedules, and online weather data is less accurate than a sited sensor, which "possibly accounts for" most online-weather products also using load compensation ([BRE 2017 pp.12–14](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)). A sited outdoor sensor sits out of direct sun, away from windows, vents and balconies, at 2–2.5 m.

### 4.3 Heating curves and room influence

Commercial curves are defined by slope and parallel shift. The Siemens RVL470 defines slope s = (T_flow at −5 °C − T_flow at +15 °C)/20 K and uses a room-influence gain from 0 to 20 ([Siemens RVL470](https://www.manualslib.com/manual/958756/Siemens-Rvl470.html?page=35)). Vaillant describes the same idea as "slope" and "level" ([Vaillant](https://www.vaillant.com/en/advice/help-center/heating-curve/)). Synco approximates the effect of a room-setpoint change as Δflow ≈ Δroom × influence × (slope + 1) ([Siemens Synco700](https://manualslib.com/manual/1226667/Siemens-Synco700-Rmk770.html?page=163)). The open-source SAT curve is `base + (coef/4)·[4(T_target−20) + 0.03(T_out−20)² − 0.4(T_out−20)]`, with base 27.2 °C for radiators and 20 °C for underfloor, and coefficient 2.0. At a 20 °C target that gives **37.2 °C flow at 0 °C outside and 46.7 °C at −10 °C** ([SAT heating_curve.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/heating_curve.py)). SAT then sends `max(curve + PID, 10 °C)`, smoothed ([SAT climate.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/climate.py)).

### 4.4 PI(D) tuning for a slow plant

Room temperature behaves like a first-order system with dead time and a time constant of hours. SAT's field-used defaults are a useful anchor ([SAT pid.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/pid.py); [const.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/const.py)). It uses a **1 min sample time and a 0.1 K deadband**, with automatic gains Kp = coef·curve/3 (radiators), which comes to about 13–25 °C of flow per K of room error. Ki = Kp/8400 s⁻¹, an integral time of about 2.3 h, and Kd = 588·Kp, a derivative time of about 10 min. The integral is clamped to ±curve value and updates only inside the deadband. The derivative is double-EMA filtered and used only outside it. ESPHome's documented example (`kp 0.4, ki 0.004` on a 20–65 °C output) is in the same range ([ESPHome](https://esphome.io/components/opentherm/)). SAT asks for a room sensor with 0.01 °C resolution and a 30 s refresh. The SHT40 meets that, provided board self-heating (display, ESP, camera) is calibrated out. Standard practice adds derivative-on-measurement, to avoid kicks at schedule changes, and an integral freeze during saturation, DHW priority, PWM and anti-cycle lockout.

### 4.5 Setback and optimum start: shallow beats deep on a condensing boiler

DOE finds about 1 % saving per °F of 8-hour night setback for hydronic systems. It advises against setback in very well-insulated homes ([DOE Building America](https://www.energy.gov/sites/prod/files/2014/07/f17/ba_case_study_hydronic_setback.pdf)), and NREL studied the conflict between fast recovery (high flow) and condensing ([OSTI 1126299](https://www.osti.gov/scitech/servlets/purl/1126299)). Heat Geek recommends setbacks of **up to about 3 K for gas boilers** rather than off ([Heat Geek](https://heatgeek.com/articles/why-not-to-zone-heat-pumps-or-boilers)). Commercial optimum start (Nest True Radiant, tado Early Start, Netatmo Auto-Adapt) learns the heat-up time and starts early, with a cap on the preheat duration ([Google Nest](https://support.google.com/googlenest/answer/9250165); [tado](https://support.tado.com/en/articles/3478195)). The design consequence is a 1–3 K setback with a recovery whose flow setpoint is capped, for example at curve + 10 K. Start time should come from a learned heat-up rate rather than a fixed boost.

---

## 5. Short cycling is the dominant real-world loss, and the 4.3 kW floor makes it inevitable without explicit rules

UCL's analysis of 221 UK combis found a **median of 53 starts/day**, and 70 % of space-heating starts averaged under 10 minutes. The boilers "despite available modulation are unable to prevent rapid on-off cycling" ([Bennett, Elwell, Oreszczyn, UCL](https://discovery.ucl.ac.uk/10054754/1/StartStop_BG_Paper_BSERT_v4_final_main_updated_figures_references.pdf)). The lab penalty per burn is steep:

| Burner on-time | 3600 s | 180 s | 120 s | 60 s | 30 s | 10 s |
|---|---|---|---|---|---|---|
| Change in gross efficiency | 0 | −1.5 % | −2.3 % | −4.1 % | −6.8 % | −11.8 % |

Unburnt hydrocarbons run about **120×** higher at about 150 s cycles than near steady state ([UCL Table 1](https://discovery.ucl.ac.uk/10054754/1/StartStop_BG_Paper_BSERT_v4_final_main_updated_figures_references.pdf)). A 60–180 s burn pattern therefore costs as much as the entire SAP compensation gain at radiator temperatures. When demand falls below the minimum output, the return temperature climbs until the boiler hits its limit and cycles ([UCL](https://discovery.ucl.ac.uk/10054754/1/StartStop_BG_Paper_BSERT_v4_final_main_updated_figures_references.pdf)). The Tera's floor is 4.3 kW. A house with, say, a 10–12 kW design loss drops below that whenever it is mild outside. Practitioner and vendor guidance asks for **2–3 cycles per hour, ≤5 acceptable** ([RF MacDonald](https://www.rfmacdonald.com/boiler-white-papers/hydronic-boilers/effects-of-ultra-high-turndown-in-hydronic-boilers); [Cleaver-Brooks](https://www.globalspec.com/CleaverBrooks/ref/UltraHighTurndown.pdf)), and about 10 minutes of minimum run so the return reaches condensing temperature ([HeatingHelp](https://forum.heatinghelp.com/discussion/comment/1318212/), anecdotal).

Immergas boilers show the problem plainly. On a Victrix Omnia driven by a DIYLESS thermostat, the burner lit at about 38 % for about 20 s, cut out at setpoint +5 K and then waited about 3 minutes. The user "cannot really modulate under 40 degrees… for any reasonable amount of time" and worked around it by raising t0 to 30–35 °C and lowering ignition fan speed S2 ([diyless #34](https://github.com/diyless/opentherm-thermostat3/issues/34); [#31](https://github.com/diyless/opentherm-thermostat3/issues/31#issuecomment-3744691452)). The Tera's t4 (probably 180 s) and t5 ramp (probably 180 s) define the same behaviour.

SAT's low-load scheme is the most complete open reference ([SAT pwm.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/pwm.py); [README](https://github.com/Alexwijn/SAT)). It first calibrates an **overshoot protection value (OPV)**, the stable flow temperature the boiler reaches at minimum modulation, with defaults of 62 °C for radiators and 45 °C for underfloor. When the computed setpoint falls below the OPV, it switches to **duty-cycle PWM**. The duty is d = (requested − base)/(flow − base). By default it allows 4 cycles per hour on a 900 s base period, with a 180 s minimum ON or OFF, d < 10 % meaning stay off, and periods stretched to 30 min. During ON the setpoint is held at **flow − 3 °C**, which pins the burner at minimum fire instead of letting it overshoot and cut out. SAT's flame-health classifier calls a modulating boiler HEALTHY at ≤2 cycles/h with ≥48 % duty, and SHORT_CYCLING when the median on-time is under 162 s at more than 3 cycles/h ([SAT flame.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/flame.py)). Note one discrepancy in SAT itself: the README says calibration uses MM = 0 and CS = 75 °C, while the code uses modulation 100 % and the per-system setpoints ([overshoot_protection.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/overshoot_protection.py)). ID 14 is the second lever, but boilers differ. One answered UNKNOWN-DATAID. A tado that wrote ID 14 every 6 s stopped the boiler modulating, and another boiler stuck at low flame until ID 14 = 100 % was written ([tado community](https://community.tado.com/en-gb/discussion/19468/adding-new-id-to-opentherm-communication); [domoticaforum](https://www.domoticaforum.eu/viewtopic.php?p=98807)). Commercial products that pin modulation at 100 % and move only the setpoint are a recurring short-cycling complaint ([Netatmo feature request](https://helpcenter.netatmo.com/hc/en-us/community/posts/33120747285266)).

The lever ladder for the Tera therefore runs as follows. First, lower the flow setpoint toward the measured minimum-fire steady temperature. Second, hold the setpoint just below the actual flow during a burn. Third, below minimum output, run 15–30 min duty cycles with ≥10 min burns, ≤3 starts/h and a minimum off-time above t4. Fourth, cap ID 14 only after confirming the boiler honours it. Last, keep the reference radiators open so system water volume stays high. The underfloor circuit's mass also lengthens cycles.

---

## 6. DHW belongs to the boiler: tank NTC on terminals 36–37, and the master schedules the enable bit

The Tera Plus is built for one DHW arrangement. A tank sits on the boiler's internal 3-way valve with an **NTC probe on 36–37**. The boiler applies **DHW priority**, charging the tank at **DHW setpoint + A4 (default 25 K)** when the tank falls **A5 (default 3 K)** below setpoint, and returns to CH after t3 ([V1 manual §3.11, p.56–57](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)). The cross-manufacturer recommendation agrees: the boiler's own diverter plus cylinder sensor ("Option A") lets the master see and set DHW through the enable bit, ID 56 and the status bits. A tank thermostat on boiler terminals ("Option B") moves the temperature to the tank stat. S-plan zone valves ("Option C") make DHW look like CH demand to the boiler. Option C fights low-temperature CH and should be avoided in an OpenTherm design ([Worcester Bosch literature](https://worcester-bosch.co.uk/support/literature/download/6720816348); [tado community](https://community.tado.com/en-gb/discussion/comment/45099)).

Three Tera-specific points shape the firmware. First, with **P7 = 2 the DHW setpoint "stays on the boiler panel"**, so ID 56 writes may be ignored. The Nest on a Tera could set only the DHW maximum ([V2 manual p.80](https://gimli.freetls.fastly.net/tavolla/Products/Attachments/40503/inst.pdf); [plcforum](https://www.plcforum.it/f/topic/304253-immergas-vitrix-tera-nest-heat-link/)). The master's reliable DHW lever is therefore the **DHW-enable bit** (ID 0 HB bit 1), which can schedule charging windows, plus reading ID 26 if the tank temperature is exposed. Second, the boiler has **no automatic anti-legionella**. The tank exceeds 60 °C only when a suitable Immergas control triggers it ([V1 manual, warnings](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)). A weekly 60 °C+ cycle must come from ID 56 if it is writable, or from a manual or panel routine. HSE guidance is storage at ≥60 °C and distribution reaching 50 °C within 1 minute ([HSE](https://hse.gov.uk/legionnaires/hot-and-cold.htm)), and manufacturer practice is a weekly whole-tank pasteurisation ([Heat Geek](https://heatgeek.com/articles/legionella-and-water-temperature-what-you-need-to-know)). Third, the default A4 = 25 K means a 50 °C tank is charged with **75 °C flow**, which never condenses. Lowering A4 toward 10–15 K, where the coil is large enough, shortens the non-condensing period at the cost of longer charges. The installer should size the coil with that in mind.

For the control loop, DHW priority means CH flow collapses during a charge and ID 17 may exceed 100 % ([jotuera hub](https://raw.githubusercontent.com/jotuera/immergas-modbus-bms-nasa-opentherm-hub/main/docs/opentherm-hub.md)). The PI integral must freeze, and anti-cycling counters must not count DHW burns as CH starts, whenever the slave DHW-active bit is set. Scheduling charges right after a CH run, or at low CH demand, avoids extra starts. If a non-Immergas tank is chosen, its sensor must be an NTC electrically compatible with the B2 input. The B2 type is not given in the notes, so the installer must confirm it.

---

## 7. Hydraulics set the ceiling: one flow temperature, a thermostatic underfloor mixer and TRVs that must not fight the curve

### 7.1 Headers, bypasses and minimum flow

A low-loss header or close-coupled tees decouple several pumps. If boiler-side flow exceeds system-side flow, hot supply short-circuits into the boiler return and raises it ([Heat Geek: low loss headers](https://www.heatgeek.com/low-loss-headers/); [HeatingHelp](https://forum.heatinghelp.com/discussion/142026/low-loss-header-am-i-understanding-this-correctly)). Underfloor needs about 3× the flow of radiators, which is the usual reason it gets its own pump and separation. Modern internal pumps with about 7 m of head often make a header unnecessary. Automatic bypass valves, and the Tera's **factory-open internal bypass**, have the same return-warming effect whenever they pass flow, and a weather compensator does not prevent it ([Drayton ABV](https://draytoncontrols.co.uk/sites/default/files/2025-04/D30-9%20Automatic%20By%20Pass%20Valve%20Datasheet.pdf); [diynot](https://www.diynot.com/diy/goto/post?id=2138928)). Danfoss-cited field data shows boilers condensing **about 5 % of the time unbalanced versus about 80 % balanced** ([HVP/Danfoss](https://www.hvpmag.co.uk/Danfoss-outlines-the-benefits-of-heating-system-balancing/13075)). The installer's choices on header, bypass setting, pump mode and balancing therefore bound the achievable efficiency more than any firmware.

### 7.2 Underfloor behind a thermostatic mixer

A fixed-point thermostatic mixing kit (typically 20–60 °C, with its own pump) can only mix *down*. When the boiler flow falls below its setpoint on mild days, the hot port opens fully and the floor gets whatever the boiler delivers ([Altecnic/Caleffi UFH unit](https://altecnic.co.uk/wp/wp-content/uploads/2024/10/UFH-Compact-Unit-Data-Sheet.pdf); [Emmeti](https://emmeti.com/en/service-area/faq-frequently-asked-questions/mixing-units)). In cold weather the mixer throttles and recirculates, and the cool UFH return (about 30 °C) actually helps condensing. OpenTherm CH2 (ID 8) would allow an independent setpoint, but "very few boilers allow you to control the second zone" ([HA community](https://community.home-assistant.io/t/opentherm-gateway-thermostat-with-full-integration-for-home-assistant/617228/209)). The Tera documents nothing of the kind, so probe ID 3 bit 5 and expect none. With one flow setpoint, the master must send **max(radiator need, UFH need + about 5 K)** while the floor is in demand, and the UFH mixer setpoint should be as low as the floor allows. A motorised, weather-compensated mixer would decouple the two later. Screed and pipe protection must be an independent mechanical high-limit on the UFH flow (typically 50–55 °C, cutting the UFH pump), never firmware alone. The Tera manual likewise calls for a safety thermostat at least 2 m downstream for direct low-temperature use ([V1 manual §1.9](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)).

### 7.3 Aggregating rooms and living with TRVs

Honeywell evohome uses **max-demand**. Each actuator reports 0–200, zone demand is the maximum of its actuators, boiler demand is the maximum of all zones, and that maps to an OpenTherm flow setpoint or to TPI ([evohome protocol wiki 3150](https://github.com/zxdavb/The-Evohome-Protocol/wiki/3150:-Zone-Heat-Demand)). Users complain that demand jumps to 100 % whenever any zone does ([HA evohome thread](https://community.home-assistant.io/t/honeywell-ch-dhw-via-rf-evohome-sundial-hometronics-chronotherm/151584?page=90)). SAT's "Comfort" mode uses the highest room error, while "Eco" uses only the main thermostat ([SAT](https://github.com/Alexwijn/SAT)). Versatile Thermostat's central-boiler manager switches on above a count or power threshold, with an activation delay ([VT](https://github.com/jmcollin78/versatile_thermostat/blob/main/custom_components/versatile_thermostat/feature_central_boiler_manager.py)). Cheap TRVs report mostly 0 % or 100 % valve positions, which makes position feedback weak ([HA community: Moes](https://community.home-assistant.io/t/moes-trv-ts0601-valve-position-overheating/525998)).

TRVs fight compensation: closing early is "braking and accelerating at the same time." Heat Geek's worked example shows that setting 2 of 4 rooms to 18 °C cuts heat loss only about 4–6 %, yet raises the active radiators' mean water temperature from about 46 °C to about 51.5 °C, and turning rooms fully off pushes flow to about 64 °C ([Heat Geek](https://heatgeek.com/articles/why-not-to-zone-heat-pumps-or-boilers)). The standard fixes are three. Keep a **reference room** whose radiator never closes and which holds the control sensor ([BuildHub](https://forum.buildhub.org.uk/topic/30469-are-trvs-really-a-good-thing), anecdotal). Set TRVs 1–3 K above target, as limiters, under weather compensation ([diynot](https://www.diynot.com/diy/goto/post?id=5657920)). And drop the CH-enable bit, rather than just lowering the setpoint, when every non-reference valve is closed. For later phases, the evidence favours **capped, deadbanded boosts** from secondary rooms (for example ≤ +5 K on TSet after N minutes of sustained error) over raw max-demand, together with real hydraulic balancing using lockshields or dynamic valve inserts.

---

## 8. Reference implementations converge on one recipe: curve + PI trim, a keep-alive and anti-cycling

| Project | What it is | What to copy | Source |
|---|---|---|---|
| **OTGW** (Schelte Bron) | Man-in-the-middle gateway; monitor mode GW=0, gateway GW=1 | Setpoint vigilance (refresh ≤60 s or drop); 3× Unknown-DataId ⇒ unsupported; wait for 10 ms bus idle after errors; watchdog reset ⇒ hardware pass-through | [OTGW docs](https://otgw.tclcode.com/firmware.html) |
| **ESPHome OpenTherm** | Master-only hub; generic `pid` climate on `t_set` | Status every cycle, config once at init; **use `ch_enable` to stop heating** when the setpoint minimum is above 0; frame rewrite hooks for odd boilers | [ESPHome](https://esphome.io/components/opentherm/) |
| **ihormelnyk library** | Arduino/ESP interrupt library | Request/response state machine, 1 s timeout, 100 ms post-response gap; replace bit-banging with hardware timing | [OpenTherm.cpp](https://github.com/ihormelnyk/opentherm_library/blob/master/src/OpenTherm.cpp) |
| **jpraus arduino-opentherm** | OT 2.2 master + slave shield (OTGW-derived) | Gateway topology for passive logging | [jpraus](https://github.com/jpraus/arduino-opentherm) |
| **DIYLESS** | Master shield (MCU-powered, polarity-free); Thermostat 3 firmware | Hardware is proven on Immergas; firmware shows Immergas cycling issues | [DIYLESS (mirror)](https://diyless-old.duckdns.org/product/master-opentherm-shield); [diyless #34](https://github.com/diyless/opentherm-thermostat3/issues/34) |
| **SAT** | HA integration: curve + PID → ID 1 | Auto-scaled gains, OPV calibration, low-load PWM with flow − 3 °C hold, flame-health classifier, dynamic minimum from return temperature | [SAT](https://github.com/Alexwijn/SAT) |
| **Versatile Thermostat** | HA TPI: on% = c_int·ΔT_in + c_ext·ΔT_out | TPI as the on/off fallback mode; Auto-TPI; central-boiler thresholds | [VT prop_algo_tpi.py](https://github.com/jmcollin78/versatile_thermostat/blob/main/custom_components/versatile_thermostat/prop_algo_tpi.py) |
| **Better Thermostat** | HA TRV wrapper; external-sensor calibration (Default/Aggressive/MPC/TPI/PID) | Phase 3 TRV calibration; it does not drive the boiler | [BT const.py](https://github.com/KartoffelToby/better_thermostat/blob/master/custom_components/better_thermostat/utils/const.py) |
| **OTGateway** (Laxilef) | Most complete OSS standalone ESP master; equitherm + PID + hysteresis | **Immergas fix (0xCA)**, ID 57 sync option, emergency mode on Wi-Fi/MQTT/sensor loss, 5 retries before disconnect | [OTGateway](https://github.com/Laxilef/OTGateway) |
| **Commercial** (Nest, tado, evohome, Netatmo, Vaillant) | Proprietary | Curve + room trim + learned preheat + user flow cap; Nest is ErP Class VI on OT | [Nest True Radiant](https://support.google.com/googlenest/answer/9250165); [tado flow-temp optimisation](https://community.tado.com/en-gb/discussion/24396/announcement-flow-temperature-optimization-now-available-for-v-generations-and-tado-x-opentherm) |

Two lessons recur. A plain PID on flow setpoint, with no feed-forward and no low-load handling, "is the minimum viable approach", and SAT exists largely because that cycles badly. Explicit management of modulation and minimum-fire behaviour is the differentiator that commercial products often skip. The architectural lesson is just as clear. SAT, a Home Assistant integration, recommends a physical primary thermostat as backup in case HA fails ([SAT](https://github.com/Alexwijn/SAT)). This project's advantage is that the control loop lives in the thermostat itself, and that advantage should be kept: HA advises, the MCU decides.

---

## 9. Learning should start as a 1R1C model and a logging habit, not as MPC or reinforcement learning

### 9.1 Grey-box RC models are enough for one house

Bacher and Madsen's likelihood-ratio forward selection over stochastic RC models is the reference method for choosing model order ([DTU Orbit](https://orbit.dtu.dk/en/publications/identifying-suitable-models-for-the-heat-dynamics-of-buildings/)). CTSM-R implements continuous-discrete Kalman-filter maximum likelihood ([ctsmr paper](https://ar5iv.arxiv.org/html/1606.00242)). Recent work fitted 1-state (≈1R1C) and 2-state (≈2R2C) models with CasADi + IPOPT on **21 days of 10-minute data**. It reached about **89 % accuracy (100 − MAPE)**, and the 1-state model was as good as the 2-state one. Longer windows did not help, and the data was simulated rather than measured ([arXiv 2510.21044](https://arxiv.org/html/2510.21044v1)). For this house, a discrete ARX form, T[k+1] = a·T[k] + b₁·T_out + b₂·u + b₃·I_sol + c, can be fitted by **recursive least squares with forgetting** on the P4 for negligible compute. Its time constant τ = RC is the single most useful learned quantity, because it drives optimum start and setback depth. Identifiability is the trap. A well-regulated room barely moves, so setbacks, setpoint steps and free-floating cool-downs supply the excitation, and solar and internal gains are the main unmodelled disturbances. These modelling recommendations are engineering judgement, not cited results.

### 9.2 MPC returns are real but inflated and expensive

A 2025 review of MPC and RL field demonstrations covered 24 residential papers and 52 residential tests. After reliability filtering, average savings were **22 % energy and 16 % cost**, but **71 %** of demonstrations used protocols that may be unreliable. Tests shorter than a week, simulation baselines and partial-room control inflate savings, and only 3 of 23 residential papers reported confidence intervals. One residential deployment took **190 engineer-days** ([arXiv 2503.05022](https://arxiv.org/html/2503.05022v6)). No reviewed residential example was clearly a gas boiler, and much of the MPC benefit comes from time-of-use load shifting, which a flat gas tariff does not offer. A tuned weather-compensated PI with optimum start probably leaves MPC only single-digit percent, which would be hard to prove. RL needs either months of comfort-risking exploration or a simulator, which means building the RC model first. The priority ladder is therefore: compensation and modulation, then anti-cycling, then optimum start from a learned τ, then an adaptive curve nudged by a persistent integral term, then forecast feed-forward (mainly for the slow underfloor), and only then MPC.

### 9.3 Split the work: the MCU owns safety and control, HA owns batch learning

The P4 should run the bus, clamps, watchdog, fallbacks, PI and curve, anti-cycling, optimum start and RLS, with bounded parameters persisted in NVS. Python next to HA (scipy, CasADi, cvxpy/OSQP) should do weekly re-fits, KPI computation, A/B scheduling and forecast feed-forward. It pushes only **range-checked** curve or τ parameters back to the MCU. The thermostat must run indefinitely without HA ([arXiv 2510.21044](https://arxiv.org/html/2510.21044v1); [SAT](https://github.com/Alexwijn/SAT)). TinyML adds nothing over a physics model for this low-dimensional plant.

### 9.4 What to log from day one

Without a pre-change baseline and a gas reading, no efficiency claim can be verified later. Reliable field evaluations need same-building, weather-normalised baselines and multi-week tests ([arXiv 2503.05022](https://arxiv.org/html/2503.05022v6)). The recommended schema below is engineering judgement built on the notes.

| Signal | Source | Rate | Purpose |
|---|---|---|---|
| Room temp + RH (raw 0.01 °C), sensor ID, calibration offset | SHT40 (+ later sensors) | 30–60 s | Control, model, comfort KPI; self-heating correction |
| Room setpoint, mode, schedule state | Thermostat | On change | Separate user-driven from control-driven changes |
| TSet sent, value ACKed, and controller internals (base or curve, P, I, PWM state, limits active) | Thermostat | Every write | Debug and tuning; ACK reveals clamping |
| ID 0 flags (CH, DHW, flame, fault, diag) | Boiler | Every cycle + edges | Burner starts and run lengths from flame edges |
| ID 25 flow, ID 28 return, ID 17 modulation | Boiler | 10–30 s | Condensing and modulation KPIs, energy proxy |
| ID 18 pressure, ID 26 tank, ID 27/33 if any | Boiler | 1–5 min | Leaks, DHW, flue |
| ID 5 / ID 115 fault codes; E-codes noted by hand | Boiler | On change | Fault mapping |
| ID 15, 48, 49, 57, 3, 125, 127, plus support map | Boiler | Boot | Configuration record |
| ID 116–123 counters | Boiler | Hourly | Cross-check edge counts |
| Outdoor temp (source labelled: HA forecast, HA actual, own sensor) | HA / sensor | 5–15 min | Curve, HDD normalisation |
| Irradiance or cloud cover (PV power as proxy) | HA | 5–15 min | Solar-gain term |
| **Gas meter** (pulse or smart-meter port) | Meter | Pulses or 5 min | Ground-truth energy; split CH/DHW by flags |
| Window, occupancy, camera motion | HA / device | On change | Disturbance terms |
| OT error and timeout counts, reply latency, RSSI, reboots, reset reason, firmware/strategy ID | MCU | 5 min / on change | Reliability KPIs, A/B tagging |

Store everything in UTC with NTP time. Keep a few days in an on-device ring buffer for HA or Wi-Fi outages, and use long-term storage such as InfluxDB or VictoriaMetrics rather than HA's short default recorder retention. That retention figure is general knowledge, not verified in the notes.

---

## 10. Efficiency KPIs need a gas meter, degree-day normalisation and week-long A/B blocks

The KPIs that track condensing-boiler performance are these. First, **gas per heating-degree-day**: a change-point energy-signature regression, E_day = a + b·HDD(T_base), compared on its slope b. Second, **burner starts per day and per burner-hour**, median run length and the share of runs under 10 min. Third, **% of flame-on time with return below 55 °C** (strict: below 50 °C) and the flame-weighted mean return. Fourth, mean modulation while firing and % of time at minimum (an oversizing signature). Fifth, **comfort degree-hours** below setpoint − 0.5 K and above it, which guard against "savings" that are only a colder house. The condensing threshold is placed at about 54–55 °C return ([Deppmann](https://www.deppmann.com/wp-content/uploads/2016/10/RLD_MMM_%E2%80%98Tis-the-Season-for-Condensing-Boilers.pdf)). Cycling targets of ≤2–3/h, with ≤5/h acceptable, come from vendor and trade sources ([RF MacDonald](https://www.rfmacdonald.com/boiler-white-papers/hydronic-boilers/effects-of-ultra-high-turndown-in-hydronic-boilers)). A DOE field test of a boiler cycling controller found savings scaled with how oversized the boiler was ([DOE Building America](https://www.energy.gov/sites/prod/files/2014/11/f19/ba_case_study_boiler_load_monitoring_controls.pdf)). That points to the Tera's 4.3 kW floor against the house's mild-day load as the number to watch.

An energy estimate from modulation, P ≈ P_min + RelMod·(P_max − P_min), is useful only after calibration against the meter. RelMod semantics are boiler-specific (Section 2.2), and the researcher's estimate is ±10–30 % uncalibrated. For A/B tests, alternate strategies in blocks of **≥7 days, ideally 2 weeks**. Discard 1–2 days after each switch for thermal-mass carry-over, keep comfort setpoints identical, tag every record with the strategy ID, and fit the energy signature with a strategy dummy to obtain a confidence interval. Shorter tests are the documented route to inflated claims ([arXiv 2503.05022](https://arxiv.org/html/2503.05022v6)).

---

## 11. Safety: the boiler owns the hazards, the firmware owns the failure direction

The boiler keeps its certified safety chain: flame supervision, safety thermostat, flue thermostat, pressure switch, frost protection and a lockout after 5 resets (E08) ([V1 manual §2.6](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)). OpenTherm only lets the master request a flow temperature and enables, and the boiler "only needs to control to the value" ([spec §2.1](http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf)). That positions the DIY thermostat as **Class A software under IEC/EN 60730-1 Annex H**, "not intended to be relied upon for safety", but only if it is never the sole protection against a hazard ([Microchip: software classes](https://onlinedocs.microchip.com/oxy/GUID-A619C762-0FA4-4AA9-B164-91996365FAF7-en-US-9/GUID-24711A10-AEB3-402F-8B75-B35422232E0F.html)). Hence the layering. Layer 1 is the boiler's certified controls. Layer 2 is the independent hardware UFH high-limit and mixer. Layer 3 is firmware clamps. Layer 4 is HA. Layers 3 and 4 never protect screed or pipes.

On this boiler, the firmware's real safety job is choosing the failure direction:

| Failure | Generic OT risk | Tera behaviour (documented or inferred) | Mitigation |
|---|---|---|---|
| Firmware hang, line idle | Short-circuit rule ⇒ heat demand after 5–15 s | Unknown with P7 = 2; E31 likely | Hardware watchdog; test both behaviours |
| MCU unpowered or shield dead | Same | E31; after power cycle **CH refused** | NC relay across 44/40–41 driven by a watchdog ⇒ boiler on own setpoint (verify no power cycle needed) |
| Missed replies | Frame loss routine | 17.6 % misses seen on an Immergas | 1 s timeout, retries, declare loss only after ~5 s; never stop the cadence |
| Room-sensor failure (I²C NACK, CRC, stuck, out of range, implausible rate) | Overheat or underheat | — | Fall back: HA sensor → outdoor curve → fixed 40–45 °C with duty limit; alert; never "assume cold and run flat out" |
| HA or Wi-Fi loss | — | — | Local schedule and curve; HA-pushed parameters time out to defaults |
| CH-off ignored | Keeps firing to last setpoint | Reported on Immergas | Clear the CH bit **and** send ID 1 = 10 °C |
| Bad parameter from HA or OTA | Runaway setpoint | — | MCU-side range checks; dual-partition OTA with rollback, marked valid only after OT and sensor self-test |

Further rules belong in the firmware. Frost protection must be independent of schedules and HA: if the room falls below 5–7 °C, enable CH at a low setpoint. Every value sent should be clamped, TSet to [t0-equivalent, configured max] with radiators at 55–60 °C and anything touching UFH alone at ≤45–50 °C. Write ID 56/57/14 only on change, because EEPROM wear is plausible but unconfirmed. Never auto-reset lockouts in a loop. Alarm on pressure below about 1.0 bar, on fault flags and on ΔT anomalies. The existing test firmware turns heating **off** after 30 s of SHT40 loss. That is safe against overheating, but on this boiler it should become "keep the bus alive, CH at a conservative fixed setpoint", so that a sensor fault in January does not chill the house. The ErP view adds context rather than certification. OT modulation plus a room sensor is Class V (+3 %). Adding weather compensation makes it Class VI (+4 %), Nest's rating on OT. Three or more aggregated room sensors make it Class VIII (+5 %) ([Nest ErP sheet](https://nest.com/support/images/misc-nest-thermostat-eu/064-00025-GB-A_A2_ErP_English.pdf); [EU space-heater packages](https://energy-efficient-products.ec.europa.eu/product-list/space-heaters/packages-space-heaters_en)).

---

## 12. Recommended Phase 1 control design

This design combines the cited findings with engineering judgement. Every numeric default is a starting point to confirm in the bench tests of Section 14.

**Boiler and wiring prerequisites.** Buy a **Victrix Tera V2 24 Plus EU**. Have the installer set **P7 = 2**, leave the outdoor sensor off the boiler for now (or fit it and test its interaction), and set t0 to about 30 °C and t1 to about 60 °C, so the boiler itself caps what the thermostat can ask for. Set P4 = 0 (pump follows demand) and keep A3 at constant ΔT of 15–20 K. Close the internal bypass as far as the manual's minimum-flow requirement allows. Lower A4 toward 15 K if the tank coil supports it. Wire the DIYLESS shield (P4 GPIO29 in, GPIO30 out) to 44/40–41 with X40 removed. Add a normally-closed fallback relay across the same pair, held open by a hardware watchdog that the firmware must toggle. Put a mechanical high-limit stat on the UFH flow that cuts the UFH pump.

**Bus layer.** Use a dedicated high-priority task with hardware-timed Manchester transmit and edge-timestamped receive. Each roughly 1 s cycle runs **ID 0 (READ, HB = CH|DHW flags, LB = 0xCA, configurable)**, then **ID 1 WRITE**, then one rotating slot, with 100 ms gaps and a 1 s reply timeout. The rotating slots carry ID 25, 17, 28, 18, ID 5 (when the fault bit is set), 26, the ID 57 mirror write, and hourly the counters 116–123. Start-up runs ID 0 ×N, then ID 3, ID 2 write (flags 0, MemberID 0), ID 124 = 2.2, ID 125/127, ID 15, ID 6, ID 48/49, then probes of the optional IDs. An ID that returns UNKNOWN-DATAID or DATA-INVALID three times is marked unsupported and re-probed hourly and after reconnect. The link counts as lost only after about 5 s of consecutive failures, which raises an alarm while transmission continues. **The cadence never stops**, including during OTA and while the Wi-Fi stack is busy.

**Control law (Phase 1a, indoor sensor only).** Load compensation: TSet = clamp(T_base + Kp·e + I, T_low, T_max). T_base is a fixed 35 °C, or the SAT curve evaluated at an assumed outdoor temperature. Start with Kp about 10–15 °C per K, Ti about 2–3 h and no derivative. Sample every 60 s with a 0.1 K deadband. Rate-limit TSet changes to about 1 K/min. Freeze the integral during DHW-active, PWM-off phases, anti-cycle waits and saturation. Clamp T_max at 55 °C for radiators, rising toward 60 °C only if the reference room cannot reach setpoint at design weather, and T_low at the measured minimum-fire temperature. While the UFH circuit is in season, floor TSet at UFH mixer set + 5 K, or accept a cooler floor on mild days. **Phase 1b** adds the outdoor temperature from HA as curve feed-forward: SAT-style curve, coefficient fitted, PI integral bounded to ±10 K around the curve. It falls back automatically to 1a when HA data is stale for more than 30 min.

**Low-load and anti-cycling.** During a burn, once the flame is stable, hold TSet at about flow − 2 to 3 °C so the burner sits at minimum fire rather than overshooting to the +5 K cut-out. When the PI asks for less than the minimum-fire temperature, switch to duty-cycle mode: 20–30 min periods, burns of at least 10 min, off-time at least t4 + margin, **≤3 CH starts/h**, and OFF meaning the CH bit cleared plus ID 1 = 10 °C. Leave ID 14 at default until bench tests prove the boiler honours it. Never write TSP 12.

**DHW.** Drive the DHW-enable bit from a schedule of charging windows (for example morning and evening, plus a top-up after CH runs). Read ID 26 if available. Attempt a weekly ID 56 = 60–65 °C pasteurisation only if a read-back confirms writes are accepted; otherwise document a manual panel routine. Exclude DHW burns from CH KPIs and counters.

**Failsafes.** Use the task watchdog and interrupt watchdog, plus the external hardware watchdog driving the fallback relay. The sensor-failure ladder, frost floor and MCU-side validation follow Section 11. Boot into "bus alive, last-good clamped setpoint" mode.

**Logging.** Log the full schema of Section 9.4 from the first power-on, including the gas meter. Run at least two weeks in a simple, fixed configuration (for example a fixed 45 °C flow with CH-enable on/off from the room sensor) as the baseline arm before enabling PI.

---

## 13. Phased roadmap

| Phase | Hardware | Control | Learning / data | Exit criterion |
|---|---|---|---|---|
| 0 (now, pre-install) | Bench with simulated boiler | Bus stack with Immergas options; failsafe state machine; logging pipeline | Schema in place | Simulated comm-loss, sensor-loss and OTA rollback pass |
| 1a | Tera V2 + shield + fallback relay; SHT40 in reference room; gas pulse reader | Baseline arm, then load-compensating PI + anti-cycling + DHW scheduling | 2-week baseline; KPIs live | All Section 14 tests pass; ≤3 starts/h; no E31 in a week |
| 1b | + HA outdoor temperature | Curve feed-forward + PI trim (Class VI-like), stale-data fallback | Fit curve coefficient from integral history | Integral term near zero across outdoor temperatures |
| 2 | Sited outdoor sensor (own, not the boiler's) | Curve on measured T_out; 1–3 K setback; optimum start | RLS 1R1C on MCU; τ and heat-up rate; weekly batch 2R2C in HA | Recovery on time within ±15 min; A/B shows no comfort loss |
| 3 | 2+ more room sensors | Capped deadbanded boosts (≤+5 K), weights, Class VIII aggregation | Per-room models; balancing guided by data | No single room drives flow up persistently |
| 4 | Smart TRVs (external-sensor calibrated), dynamic valve inserts | Valve-position demand with thresholds; reference radiator TRV-free; CH off when all closed | Better Thermostat-style calibration | Return temp and starts no worse than Phase 3 |
| 5 | — | Forecast feed-forward (UFH), optional MPC on setpoint and curve | Offline grey-box + MPC in Python; strict A/B blocks | Statistically significant saving with CI, or stop |

---

## 14. Verify on the real boiler: tests and pass criteria

| # | Item | Why | Test method | Pass / note |
|---|---|---|---|---|
| 1 | Generation V1 vs V2 | P7 exists only on V2 | Rating plate/code (3.032933), installer menu P7 offers 0/2, record d1.9 versions | V2 with P7 = 2 set and power-cycled |
| 2 | Bus voltage | IMG BUS vs CAR-BUS | Meter on 44/40–41, X40 removed, nothing connected | About 15–18 V idle; 24–35 V = stop, not OT-compatible |
| 3 | Terminal identity and polarity | Manual says "respect polarity"; reports say 40-41 and 44-41 | Continuity 40↔44; note polarity; shield is polarity-free | Document the actual pair |
| 4 | First contact and 0xCA | Quirk necessity | Send ID 0 with LB 0x00, then 0xCA; log reply type and latency | Replies, and whether writes take effect only with 0xCA |
| 5 | Reply statistics | 800 ms window, misses | 24 h log of latency histogram and miss rate per ID | Misses <1–2 %; latency ≪800 ms |
| 6 | Identity and capabilities | Configuration | Read ID 3, 125, 127, 15, 6, 48, 49, 93–95 | Record MemberID (expect 27), min modulation %, bounds |
| 7 | Which setpoint the boiler heats to | ID 1 vs ID 57 behaviour | ID 57 = 60, step ID 1 30→45→40; then repeat without ID 57; watch ID 25, d0.4, d1.7 | Flow follows ID 1; decide whether the mirror is needed |
| 8 | ID 56/57 writability with P7 = 2 | Panel may own them | Write, read back, check panel | Note ACK vs effect |
| 9 | CH-off semantics | "Keeps firing" risk | Clear CH bit with ID 1 at 45; then clear bit + ID 1 = 10 | Burner off within seconds in both cases, or only the second |
| 10 | ID 14 honoured | Modulation lever | ID 14 = 0/30/100 during a steady burn; watch ID 17 and d1.5 fan rpm | Fan/ID 17 respond, or mark unsupported |
| 11 | ID 17 scaling | Energy proxy | Compare ID 17 against d1.5 at minimum, at P2 and during DHW | Map ID 17 to kW; confirm >100 % in DHW |
| 12 | Minimum-fire steady flow temperature (OPV) | Low-load threshold | All radiators open, mild day, low setpoint; hold TSet = flow − 3 °C | Stable temperature at minimum fire recorded |
| 13 | Cut-out overshoot and anti-cycle | Cycling model | Setpoint below steady flow; time cut-out (setpoint + ? K) and re-ignition gap | Confirm t4 = 180 s and the overshoot margin |
| 14 | Ignition burst and ramp (t5, S2) | Short-burn behaviour | Log ID 17 and fan rpm for the first 5 min after ignition | Ramp duration matches t5 |
| 15 | Pump post-run, P4 behaviour | Hydraulics and logging | Observe pump after CH off (d1.2, d1.4) | Post-run time recorded |
| 16 | Supported sensors | Poll map | Probe ID 18, 19, 26, 27, 28, 33, 34–36, 115, 116–123 | Expect ID 19 unsupported; ID 28 hoped for |
| 17 | Fault mapping | Alarms | Benign fault: unplug tank NTC (E12) or lower pressure; read ID 0 fault bit, ID 5, ID 115 | Map E-codes to OT values |
| 18 | Comm loss, MCU powered, line idle | Short-circuit vs E31 | Stop TX with the line idle low; time to E31 and whether it fires | Record behaviour and timing |
| 19 | Comm loss, line held high or MCU unpowered | Same | Repeat with the line high and with the shield unpowered | Record behaviour |
| 20 | Recovery without power cycle | Availability | Resume TX after E31 | Does CH come back without a power cycle? |
| 21 | Fallback relay | Failsafe | After E31, close the relay across 44/40–41 | Boiler heats on its panel setpoint without a power cycle (or document the needed cycle) |
| 22 | Frost function under OT | Last line | Check that stand-by with OT keeps frost protection (manual: not in "off") | Confirmed by display or log |
| 23 | DHW cycle | Priority and logging | Tank call: watch flags, CH interruption, A4 flow, t3 | DHW bit gating works; integral freeze triggers |
| 24 | Outdoor sensor interaction (if fitted) | Curve conflict | With sensor fitted, vary ID 1 | ID 1 wins, or remove the sensor |
| 25 | EMC with Wi-Fi, camera, display | Frame errors | Compare error rates camera on/off, during Wi-Fi bursts | No rate increase |
| 26 | SHT40 self-heating | Control accuracy | Reference thermometer, camera/screen on vs off | Offset recorded and applied |

---

## 15. Installer checklist

1. Is the supplied boiler a **Victrix Tera V2 24 Plus EU** (code 3.032933)? Can it be confirmed on the order and rating plate, and will you set **P7 = 2** ("Távvezérlő = 2") and power-cycle?
2. Which terminals will the remote use (44/40–41), and will X40 be removed? Will the pair be left free of any other thermostat?
3. Will you set t0/t1 (min/max CH setpoint) to agreed values (for example 30/60 °C), and what are P1/P2, t4, t5, S2 as commissioned?
4. Pump: which mode (A3 ΔT, proportional or fixed) and which A1/A2 limits? Will P4 be intermittent? Is there any external pump besides the UFH kit pump?
5. The internal bypass ships open. What setting will you use, and what minimum flow does the boiler need with zone valves or TRVs closed? Is there an external automatic bypass, and at what set pressure?
6. Is there a low-loss header, close-coupled tees or a plate heat exchanger? If so, why, and how will primary flow be kept at or below secondary flow?
7. How are the two radiator circuits separated: zone valves (then what t6) or just balancing valves? Has the system been, or will it be, hydraulically balanced, and by what method?
8. UFH kit: make/model, thermostatic or motorised, set temperature, own pump, pump-enable logic, oxygen-barrier pipe? Will you fit an independent mechanical high-limit stat on the UFH flow that cuts the UFH pump?
9. DHW tank: Immergas tank unit (80/120/200 L) or another tank? Is the sensor an NTC on 36–37 with R8 removed? What is the coil rating, and can A4 run at 10–15 K without unacceptable recharge times?
10. How will legionella protection be handled, given the boiler has no automatic cycle?
11. Outdoor sensor: will one be fitted to the boiler (38–39)? We prefer not to, or we will test its interaction with the thermostat's setpoint first.
12. What is the design heat loss of the house, and the sum of radiator ΔT50 ratings per circuit? At what outdoor temperature does the load fall below 4.3 kW?
13. What is the installer password policy? Will you leave us able to read the d-menu and parameters, or will you note all parameter values at commissioning?
14. Please confirm where the boiler's safety, frost and condensate protections sit, and that frost protection stays active in stand-by under remote control.
15. If the remote fails (E31), what is your recommended recovery, and do you approve a volt-free fallback relay across 44/40–41?
16. Is the gas meter suitable for a pulse reader (reed or optical), or does it have a smart-meter port we can read?
17. Will the commissioning record include fault-code history (d0.9), software versions (d1.9) and combustion readings at min/max, for our baseline?

---

## Conclusion

The research turns a protocol project into an integration-and-failure-direction project. The hard parts are not PID gains. They are three facts. A single parameter (P7, V2-only) decides whether the boiler accepts a third-party master at all. A single undocumented byte (0xCA) may decide whether writes take effect. And the boiler's chosen failure mode (E31, CH refused) inverts the textbook OpenTherm hazard from runaway heat to no heat. That shifts the most valuable Phase 1 engineering toward the fallback relay, the never-stopping bus cadence and a bench-test campaign. The control algorithm still matters, but the cycling rules are worth as much as the control law, because the 4.3 kW floor will be the binding constraint for much of the Hungarian heating season.

The evidence also reframes the ML ambition. The measurable savings available here are a few percent from low return temperatures and long burns, and a lower setpoint can be worth several times that. Learning pays for itself first through τ-based optimum start and a self-correcting curve, but none of that can be proven without the gas-meter baseline and week-long A/B discipline that most published MPC work lacked. The cheapest high-value action today is to log everything from the first connection and to buy the V2 boiler.
