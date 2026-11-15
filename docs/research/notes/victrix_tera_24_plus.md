# Immergas Victrix Tera 24 Plus: control from a third-party OpenTherm master

Sources used most heavily:
- **[M1]** Official Immergas IT installer/user/maintenance manual, *Victrix Tera 24 Plus* (original/V1), doc STD.008065/002, 72 pp — https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf
- **[M2]** IT manual, *Victrix Tera 24 Plus V2*, doc STD.009317/003, 104 pp (Immergas document, found on a third-party retailer CDN, not on immergas.com) — https://gimli.freetls.fastly.net/tavolla/Products/Attachments/40503/inst.pdf
- English mirrors of both on manualslib: V1 https://www.manualslib.com/manual/1423061/Immergas-Victrix-Tera-24-Plus.html ; V2 EU https://www.manualslib.com/manual/3331612/Immergas-Victrix-Tera-V2-24-Plus-Eu.html (not read page by page; the IT originals were read in full in the relevant sections)

Notation: **[CONFIRMED-MANUAL]** = stated in an Immergas manual; **[COMMUNITY]** = forum/GitHub report; **[VERIFY]** = verify on the real boiler.

## 1. Variants, outputs, efficiency, ErP, gas types

### Takeaway
"Plus" is the heating-only (system) version with an internal motorised 3-way valve, made to feed a separate Immergas tank unit (80/120/200 L). There are two generations: the original **Victrix Tera 24 Plus** (2019-era manual, product label shows 2019) and the **Victrix Tera 24 Plus V2** (product code 3.032933 for "V2 24 PLUS EU"). They differ in ways that matter for OpenTherm (see section 4), so **read the rating plate or the software version (info menu d1.9) to find out which one is installed. [VERIFY]** Both give about 24 kW CH and about 28 kW to the tank, with a 4.3 kW minimum. That is about 5.6:1 modulation on CH.

### Cited Findings
- V1 is titled "Caldaie pensili abbinabili ad unità bollitore separata, a condensazione", meaning a wall-hung condensing boiler that pairs with a separate tank unit. It can be room-sealed (type C) or open-chamber (type B) — [M1 p.1](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)
- V1 technical data [CONFIRMED-MANUAL] — [M1 §4.3 p.63](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf):
  - Heat input: DHW (tank) 28.8 kW nominal, CH 24.6 kW nominal, 4.5 kW minimum.
  - Useful output: DHW (tank) 28.3 kW nominal, CH 24.1 kW nominal, 4.3 kW minimum.
  - Efficiency (NCV) at nominal/minimum: 97.8 / 95.5 % at 80/60; 106.1 / 106.1 % at 50/30; 108.2 / 108.3 % at 40/30.
  - CH adjustable range: 20–50 °C (low range) or 55–85 °C (high range). Maximum operating temperature 90 °C, maximum pressure 3 bar.
  - DHW adjustable range 30–60 °C.
  - Expansion vessel 5.8 l with 1.0 bar precharge. Boiler water content 1.8 l.
  - Available head 26.4 kPa (2.7 m) at 1000 l/h.
  - Electrical: 80 W installed, pump 41 W, EEI ≤ 0.20.
  - NOx class 6.
  - Gas category II2H3P / I3P / II2HM3P (G20, G31, G230).
- V1 output/modulation table: CH band 24.1 kW (84 % modulation on G20) down to 4.3 kW (1 %). The DHW band reaches 28.3 kW (100 %). Gas rate on G20 runs from 3.06 m³/h at maximum to 0.48 m³/h at minimum — [M1 §4.1 p.62](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)
- V1 ErP data: ηs 93 %, P4 = 24.1 kW at η4 = 87.8 % (GCV), P1 = 8.0 kW at η1 = 97.5 % (GCV), standby loss 0.057 kW. The product label shows energy class **A**, 52 dB, 24 kW — [M1 §4.5–4.6 pp.65–66](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)
- V2 technical data [CONFIRMED-MANUAL] — [M2 §4.3 p.95](https://gimli.freetls.fastly.net/tavolla/Products/Attachments/40503/inst.pdf):
  - Heat input: DHW 28.7 kW, CH 24.5 kW, 4.5 kW minimum. On G20Y20 (20 % hydrogen blend): DHW 27.1 / CH 23.2 / minimum 4.3 kW.
  - Useful output: DHW 28.0 kW, CH 24.0 kW, 4.3 kW minimum.
  - Efficiency (NCV) at nominal/minimum: 97.8 / 96.6 % at 80/60; 106.5 / 107.6 % at 50/30; 108.0 / 108.4 % at 40/30.
  - CH range 20 to 85 °C. Expansion vessel 5.8 l at 1.0 bar. Water content 3.3 l.
  - Available head 24.4 kPa at 1000 l/h. Installed electrical power 90 W.
  - NOx class 6. Categories II2H3P / II2HM3P.
  - ηs 94 %.
- An Immergas catalogue (via archiexpo) says the Tera 24 Plus pairs with a separate tank unit through an optional kit, code **3.024907** — [archiexpo catalogue, per search snippet](https://pdf.archiexpo.com/pdf/immergas/victrix-tera/146519-376409.html) (page returned 403; not read directly)
- The combi siblings are Victrix Tera 24 / 28 / 32 (instantaneous DHW). Immergas also sells a "VICTRIX TERA VIP" variant — [immergas.com product](https://immergas.com/en/product/3.029585/VICTRIX_TERA_VIP); [Tera 24 V2 product page](https://www.immergas.com/prodotto/3.032927/VICTRIX_TERA_24_V2/)

### Inferences
- CH modulation ratio is 24.1/4.3 ≈ **5.6:1**. Across the full range (28.3/4.3) it is ≈ 6.6:1. So the minimum output is 4.3 kW, about 18 % of CH nominal.
- No 50/30 minimum output in kW is published separately. At 50/30 the minimum will be a bit above 4.3 kW, because the efficiency is higher there.
- In OpenTherm terms, the relative modulation (ID17) the boiler reports probably maps to the "MODULAZIONE %" column of the output table. CH maximum is then about 84 % (P2), not 100 %. **[VERIFY]**

### Gaps
- Exact ErP class of V2 (probably A, given ηs 94 %) was not seen on a label.
- Hungarian-market specifics (immergas.hu datasheet, whether HU stock is V1 or V2) were not checked.

## 2. Hydraulics

### Takeaway
The boiler contains a **variable-speed PWM pump** (Grundfos UPM3 or UPM4 depending on build) with three modes: proportional, constant-ΔT (default 15 K) or fixed speed. It also has an **adjustable internal bypass** (factory-open), a **motorised (stepper) 3-way valve**, a 5.8 l expansion vessel and a 3 bar safety valve. The tank connects through a **tank NTC sensor (B2)** on terminals 36–37, not a tank thermostat.

### Cited Findings
- "Le caldaie vengono fornite con un circolatore a velocità variabile" (the boilers are supplied with a variable-speed circulator). Modes [CONFIRMED-MANUAL] — [M1 §1.29–1.30 pp.36–37](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf); identical text in [M2 §1.29](https://gimli.freetls.fastly.net/tavolla/Products/Attachments/40503/inst.pdf):
  - Proportional head (A3 = 0).
  - Constant ΔT (A3 = 5–25 K, default 15).
  - Fixed speed (A1 = A2, minimum 5/6 on UPM3, 7 on UPM4).
  - In DHW mode the pump always runs at maximum speed.
- The pump is PWM-controlled. If the PWM signal cable is disconnected, the pump runs at full speed without control — [M1 p.36](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)
- Bypass (part 21) leaves the factory open and is adjustable from closed to open. It "garantisce la circolazione minima di acqua in caldaia" (guarantees the minimum water circulation in the boiler) in zoned systems — [M1 p.36–37](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)
- Hydraulic schematic: [M1 §3.4 p.51](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)
  - Components: expansion vessel (5), flow NTC (8), return NTC (15), circulator (18), absolute pressure switch (19), motorised 3-way valve (20), bypass (21), 3 bar safety valve (23).
  - Connections: MU/RU = tank-unit flow/return; M/R = CH flow/return.
- Electrical schematic legend: B2 = tank sensor ("Sonda boiler", optional), M30 = 3-way stepper motor, R8 = "resistenza inibizione funz. boll." (tank-function inhibit resistor) — [M1 §3.5 p.52](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)
- Tank unit wiring: connect to terminals 36–37 and remove resistor R8 — [M1 §1.9 p.16 and p.53](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf); same in [M2 p.72](https://gimli.freetls.fastly.net/tavolla/Products/Attachments/40503/inst.pdf)
- Error 12 = "Anomalia sonda boiler" (tank-sensor fault). The info menu shows the tank temperature (d0.7). Together these confirm that an NTC probe, not a thermostat contact, is read — [M1 pp.45, 47](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)
- Compatible tank units are 80, 120 and 200 L stainless coil tanks with a magnesium anode, ready for a recirculation fitting. The manual also shows the head available for the boiler-to-tank pipework — [M1 §1.31–1.32 p.38](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)
- Tank logic parameters [CONFIRMED-MANUAL] — [M1 §3.11 p.57](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf):
  - A0 = hydraulic model (0 = instantaneous, 1 = tank; default 1).
  - A4 = flow offset added to the DHW setpoint when heating the tank (5–50 °C, default 25).
  - A5 = hysteresis for the tank call (0–15 °C, default 3).
  - t3 = DHW priority hold time after a tank charge (0–100 s, step 10, default "2").
- DHW priority: the boiler serves the tank, then switches to CH if CH is calling (t3) — [M1 p.56](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)
- Anti-legionella of the Immergas tank happens "solo se abbinata ad un'unità bollitore e attivabile tramite apposita funzione presente sui sistemi di termoregolazione predisposti" (only with a tank unit, and only when started by a function in a suitable Immergas control). During the cycle the tank exceeds 60 °C — [M1 p.~9 (installation warnings)](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)

### Inferences
- The default tank flow temperature is the DHW setpoint + 25 K, so a 50 °C DHW setpoint gives a 75 °C flow. The tank call starts at DHW setpoint − 3 K.
- The boiler has **no built-in automatic anti-legionella**. A third-party master would have to raise the DHW setpoint periodically (for example via ID56, if the boiler accepts it).
- Default t3 "2" with a 10 s step probably means 20 s **[VERIFY]**.
- No minimum flow rate is given. In practice the bypass and the pump's air/flow alarms (E27, E60, E61) protect the boiler.

### Gaps
- Pump head curve values: only charts are given, and no numbers were extracted except 26.4 / 24.4 kPa at 1000 l/h.
- Kit code for the tank-connection kit was seen only in a search snippet (3.024907); it needs confirming.

## 3. Control electronics: installer parameters and functions

### Takeaway
To open the installer menu, hold the two buttons for more than 5 s and enter a password (not published in the manual). The menu has four groups:
- **P0–P5 (V2: P0–P8)**: power and pump settings.
- **t0–t9**: temperatures and timers.
- **A0–A6**: hydraulics and pump.
- **S0–S2**: fan speeds.

A third-party thermostat controller should check these:
- **anti-cycling timer t4** (default "18", probably 180 s)
- **ramp t5**
- **CH power P1/P2** (P2 default 83–85 %)
- **flow range t0/t1** (25–85 °C default)
- **pump P4** (intermittent or continuous)

### Cited Findings
Parameter table [CONFIRMED-MANUAL, V1] — [M1 §3.11 pp.55–57](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf); V2 is the same except as noted — [M2 pp.80–82](https://gimli.freetls.fastly.net/tavolla/Products/Attachments/40503/inst.pdf).

| Parameter | Meaning | Range | Default |
|---|---|---|---|
| P0 | Maximum DHW (tank) power | 0–100 % | 100 % |
| P1 | Minimum CH power | 0–P2 | 0 % |
| P2 | Maximum CH power | 0–100 % | V1: 85 (G20), 82 (G31), 85 (G230). V2: 83 on all gases |
| P3 | Not available | – | – |
| P4 | Pump mode: 0 = intermittent (pump managed by room thermostat or remote), 1 = continuous in winter | 0–1 | 0 |
| P5 | Outdoor-sensor correction | −9 to +9 K | 0 |
| P6 | Not available | – | – |
| P7 | **V1: not available. V2: remote protocol (see section 4)** | – | – |
| P8 | Not available (V2) | – | – |

| Parameter | Meaning | Range | Default |
|---|---|---|---|
| t0 | Minimum CH setpoint | 20–50 °C | 25 |
| t1 | Maximum CH setpoint | (t0+5)–85 °C | 85 |
| t2 | Not available | – | – |
| t3 | DHW priority timer | 0–100 s, step 10 | 2 |
| t4 | **CH ignition timer (anti-cycling)**: stops the burner relighting too often | 0–600 s, step 10 | 18 |
| t5 | **CH ramp timer**: time to ramp to the set maximum power | 0–840 s, step 10 | 18 |
| t6 | Delay before CH ignition after a room thermostat (TA) or remote (CR) call, for zone valves | 0–600 s | 0 |
| t7 | Display backlight mode | – | – |
| t8 | Display content | – | – |
| t9 | Raise of the flow switch-off temperature during the first 60 s after ignition | 0–15 | 0 |

| Parameter | Meaning | Range | Default |
|---|---|---|---|
| A0 | Hydraulics (0 = instantaneous, 1 = tank) | 0–1 | 1 |
| A1 | Maximum pump speed | A2–9 | 9 |
| A2 | Minimum pump speed | 1–A1 | 6 |
| A3 | Pump mode: 0 = proportional, 5–25 = ΔT in K | 0–25 | 15 |
| A4 | Tank flow offset | 5–50 °C | 25 |
| A5 | Tank start offset | 0–15 °C | 3 |
| A6 | DHW thermostat mode (instantaneous only) | – | – |

| Parameter | Meaning | Range | Default |
|---|---|---|---|
| S0 | Minimum DHW fan speed (×50 rpm) | – | 27 |
| S1 | Maximum DHW fan speed (×50 rpm) | – | V1: 121 (G20). V2: 118 |
| S2 | Ignition fan speed (×50 rpm) | – | 44 |

- Low-temperature (direct underfloor) use: set t0/t1 for the low range and fit an optional safety thermostat on the flow pipe at least 2 m from the boiler — [M1 §1.9 p.16](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)
- Room thermostat input: a volt-free contact on terminals **44/40 and 41**, with jumper **X40** removed. "Assicurarsi che il contatto del termostato On/Off sia del tipo 'pulito'… in caso contrario si danneggerebbe la scheda" (make sure the contact is volt-free, otherwise the board is damaged). This is low voltage, not 230 V switching — [M1 §1.10 p.17](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)
- Outdoor sensor (optional kit, B4) [CONFIRMED-MANUAL] — [M1 §1.11 p.17, §2.5 p.44](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf):
  - Wires to terminals 38–39.
  - "agisce sempre quando connessa indipendentemente dalla presenza o dal tipo di cronotermostato" (it always acts when connected, whatever room control is used).
  - The curve is chosen with the CH buttons, value 0–9 (or from CAR V2).
  - P5 corrects the reading.
  - The info menu shows the outdoor temperature (d0.6).
- With CAR V2 connected, boiler settings are made from the CAR V2. The boiler keeps reset, off and the display. Putting the boiler "off" makes the CAR V2 show "ERR>CM" — [M1 §2.5 p.44](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)
- Info menu (d-codes), useful for checking what an OT master sees — [M1 §2.7 p.47](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf):
  - d0.1 flame current; d0.2 flow temperature; d0.4 CH set; d0.5 DHW set; d0.6 outdoor; d0.7 tank; d0.8 return.
  - d0.9 last 8 errors.
  - d1.2 pump speed; d1.4 pump flow (l/h ÷ 100); d1.5 fan rpm ÷ 100; d1.6 flue temperature; d1.7 calculated flow setpoint.
  - d1.9 safety and functional software versions.
- Special functions (password, from stand-by) — [M1 §3.12–3.20 pp.58–59](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf):
  - dI: automatic de-aeration, 16.5 h.
  - FU: flue test, fan 6000 rpm for 15 min.
  - MA: maintenance, drives the fan, pump, 3-way valve and configurable relay.
  - Screed-drying: 7 days, 3 at the lower set (20–45, default 25 °C) and 4 at the upper set (25–55, default 45 °C). It requires that **no remote control is connected**.
- Chimney-sweep: forces variable power for 15 min with only the safety and limit thermostats active. Power is selectable from 0 % to P0 — [M1 §3.17 p.59](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)
- Pump anti-seize: the pump runs at least once every 24 h for 30 s. The 3-way valve also cycles after 24 h idle — [M1 §3.18–3.19 p.59](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)
- Frost protection [CONFIRMED-MANUAL] — [M1 §1.4 p.12, §2.11 p.48, §3.20 p.59](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf):
  - The pump and burner start when the boiler water drops below 4 °C. This protects down to −5 °C ambient (−15 °C with the optional kit).
  - Radiator anti-freeze: if the return water is below 4 °C, the boiler heats until it reaches 42 °C.
  - These functions stay active in stand-by, but not in "off".
- Error codes [CONFIRMED-MANUAL] — [M1 §2.6 pp.45–46](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf); V2 is the same for 31 — [M2](https://gimli.freetls.fastly.net/tavolla/Products/Attachments/40503/inst.pdf):

| Code | Meaning |
|---|---|
| 01 | Ignition lockout |
| 02 | Overheat (safety thermostat) lockout |
| 03 | Flue thermostat lockout |
| 04 | Gas-valve contact resistance |
| 05 | Flow NTC fault |
| 08 | Maximum resets reached (5, then 1 per hour) |
| 10 | Low system pressure |
| 12 | Tank sensor fault |
| 15 | Configuration error |
| 16 | Fan fault |
| 20 | Parasitic flame |
| 23 | Return NTC fault |
| 24 | Keypad fault |
| 27 | Insufficient circulation |
| 29 | Flue sensor fault |
| **31** | **Lost communication with remote** |
| 37 | Low supply voltage |
| 38 | Flame loss |
| 43 | Flame-loss lockout |
| 45 | High ΔT (power limited) |
| 47 | Burner power limited (flue temperature) |
| 51 | CAR Wireless comms loss |
| 60 | Pump blocked |
| 61 | Air in pump |
| 70 | Flow/return NTC swapped |
| 75, 76 | Flow/return NTC fault or drift |

### Inferences
- t4 and t5 defaults "18" in a "0–600 s, step 10" field mean **180 s** anti-cycle and **180 s** ramp **[VERIFY on display]**. A 180 s minimum off-time between CH ignitions limits how fast a master can cycle the burner on low load. The ramp means the burner may not reach full power for up to 3 min after ignition.
- No separate pump post-circulation parameter is listed. P4 = 0 makes the pump follow the room-thermostat or remote demand. **Post-circulation time is unknown [VERIFY]**.
- The boiler's built-in weather compensation applies whenever an outdoor sensor is fitted. If an OT master does its own compensation, it is cleaner **not** to fit the boiler sensor. The interaction of an OT ID1 setpoint with a fitted sensor is undocumented **[VERIFY]**.

### Gaps
- The installer password is not in the manual. Community sources were not searched for it.
- Outdoor-sensor kit code and the numerical curve diagram (Fig. 10) were not extracted.

## 4. OpenTherm support, terminals, Data-IDs, quirks, comm-loss behaviour

### Takeaway
Immergas does not advertise "OpenTherm". Its remote bus is **IMG BUS**, which community testers describe as near-OpenTherm: OT physical layer and frames, plus Immergas quirks. It uses terminals **44(40)/41**, the same pair as the on/off thermostat, after removing jumper X40. **This is the most important finding:**
- On the **V2** the manual has **P7 "Comando remoto"**:
  - 0 = IMG BUS (Immergas remotes such as CAR V2)
  - **2 = "comando remoto commerciale"** (a commercial, i.e. third-party, remote) on 44/41
- On the **original (V1) Tera 24 Plus**, P7 is listed as "non disponibile su questo modello" (not available).

So a third-party OT master is officially provisioned only on V2 with P7 = 2. On a V1 unit it may or may not work, and the one public report on a V1 Tera 24 Plus is a failure (timeouts). **[VERIFY which version is installed.]**

### Cited Findings
- **V2 P7 [CONFIRMED-MANUAL]**: "P7 Comando remoto – Imposta il protocollo di dialogo col dispositivo remoto (sets the protocol used to talk to the remote device). 0: IMG BUS. Selezionare questa modalità quando si connette un Comando remoto Immergas (es: CAR v2) ai morsetti 44/41. 1: Non disponibile su questo modello. 2: Selezionare questa modalità quando si connette un comando remoto commerciale ai morsetti 44/41 (con questa selezione il controllo delle regolazioni di acqua calda sanitaria e set mandata riscaldamento max resta disponibile sul pannello di caldaia). Range 0–2, default 0" — [M2 p.80](https://gimli.freetls.fastly.net/tavolla/Products/Attachments/40503/inst.pdf)
  - The bracketed note says that with P7 = 2 the DHW setting and the maximum CH flow setpoint stay available on the boiler panel.
- **V1 P7**: "P7 – Non disponibile su questo modello" (not available on this model) — [M1 p.56](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)
- Wiring diagram legend on both versions: "2 – IMG BUS (opzionale), 3 – CARV2 (opzionale)". CAR V2 goes to "morsetti 41 e 44/40… rispettando la polarità ed eliminando il ponte X40" (terminals 41 and 44/40, respecting polarity, removing the X40 jumper). Only one remote can be connected. CAR V2 is powered over the same 2 wires as the data — [M1 pp.17, 52–53](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf); [M2 p.72](https://gimli.freetls.fastly.net/tavolla/Products/Attachments/40503/inst.pdf)
- **Comm loss, E31 [CONFIRMED-MANUAL]**: "Perdita di comunicazione col comando remoto – si verifica nel caso di collegamento a un controllo remoto non compatibile, oppure in caso di caduta di comunicazione…" (occurs when an incompatible remote is connected, or when communication drops). The fix is to cut and restore power. "Se alla riaccensione non viene rilevato il Comando Remoto la caldaia passa in modalità di funzionamento locale… In questo caso non è possibile attivare la funzione 'Riscaldamento'" (if the remote is not detected after restart, the boiler runs on its own panel, and Heating cannot be switched on). So after comm loss and a power cycle, **CH is disabled** (DHW still works) until the bus or the X40 jumper is restored — [M1 p.46](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf); same in [M2](https://gimli.freetls.fastly.net/tavolla/Products/Attachments/40503/inst.pdf)
- E51 covers CAR Wireless comms loss: control falls back to the boiler panel only — [M1 p.46](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)
- [COMMUNITY] Laxilef/OTGateway wiki: "IMG-BUS from Immergas is an analogue of the OpenTherm protocol with minor changes." It lists Nike Mythos 24, **Victrix Zeus 25** and Eolo Mythos 24 4 R as working with "Fix for Immergas boilers" enabled. On the Victrix Zeus 25 the DHW flow-rate sensor is unsupported — [OTGateway Compatibility wiki](https://github.com/Laxilef/OTGateway/wiki/Compatibility)
- [COMMUNITY] **Failure on this exact model**: OTGateway issue #284, "Immergas Victrix Tera 24 Plus (original version, approximately 2021)" — [GitHub issue #284](https://github.com/Laxilef/OTGateway/issues/284)
  - The boiler normally runs with a CARV2 Wireless over IMG-BUS.
  - Setup: ESP32 + DIYLESS OT shield on 44(40)/41 with X40 removed, Immergas fix on.
  - Result: "OpenTherm connection failure and repeated TIMEOUT responses". The boiler "never sent a single response frame". ESPHome failed the same way.
  - No P7 setting was mentioned and there was no resolution as of Nov 2026 (marked stale).
- [COMMUNITY] JoTu's HA post: "TERA: P7 = 2 (third-party remote)". Measure the remote terminals open-circuit: "up to about 18 V DC" = IMG BUS (works with OT), "about 24–35 V DC" = older CAR-BUS, "which no OpenTherm device can talk to". Without CAR V2, put jumper X40 back on 40-41 and power-cycle, otherwise the boiler waits for the remote (E31) and does not heat — [HA community thread p.2](https://community.home-assistant.io/t/immergas-victrix-integration-with-ha/395191?page=2)
- [COMMUNITY] A Victrix TT measured 28 V DC open-circuit on 44(+)/41(−), which means CAR-BUS (not OT-compatible). This was not measured on a Tera — [GitHub issue #284 comment](https://github.com/Laxilef/OTGateway/issues/284)
- [COMMUNITY] jotuera hub docs (marked "Experimental – not tested on a boiler yet") — [jotuera opentherm-hub.md](https://raw.githubusercontent.com/jotuera/immergas-modbus-bms-nasa-opentherm-hub/main/docs/opentherm-hub.md):
  - Terminals: VICTRIX ZEUS 25, EXA, TERA and OMNIA use OpenTherm terminals 41/44. 42/43 is the "Super CAR" or CAR-Bus, "not OpenTherm".
  - TERA needs P7 = 2 for a third-party master. The TERA "may also wait for the CAR V2 until P7 = 2 … is set and the boiler power-cycled – not confirmed yet".
  - Immergas room-unit sequence: ID0 status with low byte **0xCA** (called "the Immergas fix"), then reads ID48/49, then writes ID1, ID56 and ID57.
  - The hub writes ID1 every cycle, ID56 every 30 s, ID57 (= CH setpoint by default) every cycle and ID14 about every minute. It reads ID17 as u8.8, noting it "can exceed 100 % during DHW". It also reads ID15, ID48/49 and ID115.
  - "Some Immergas boilers (EOLO Star, Mini EOLO) acknowledge Data-ID 1 and heat to Data-ID 57."
  - With CH off, the hub sends ID1 = 10 °C because "some boilers keep firing to the last flow setpoint with only the CH bit cleared".
  - Timing: the room unit expects an answer within 800 ms. "Immergas boilers answer in a few tens of ms".
  - A CAR V2 showing "VER PI" = IMG-BUS (works); "VER PC" = CAR-BUS.
- [COMMUNITY] Laxilef/OTGateway HA discussion — [HA thread p.1](https://community.home-assistant.io/t/immergas-victrix-integration-with-ha/395191):
  - An Immergas technician told a Victrix Tera owner "IMGBUS is almost OT, but improved a bit".
  - The same user: on OT comm loss after start-up "heater show error and it is blocked until restart".
  - An evohome R8810 (OT) on 44/41 of a Victrix 32 ErP ran with DHW stuck at 30 °C and CH at 85 °C.
- [COMMUNITY] **Nest Heat Link on a Victrix Tera 24** (plcforum, Nov–Dec 2022) — [plcforum Nest thread](https://www.plcforum.it/f/topic/304253-immergas-vitrix-tera-nest-heat-link/):
  - Wiring: OT1/OT2 to terminals **40-41**, after the factory said "OpenTherm communication è prevista" (is provided for).
  - Result: works "alla perfezione". Modulation icons appeared and flow dropped to around 45 °C to hold 19 °C.
  - Limits: only the maximum DHW can be set, and boiler errors are not shown on the Nest.
  - Another owner was told by Immergas support that the Tera 28 supports OpenTherm ("al 99 %"), that it is "Open Therm modificato" (modified), and that errors may not show on Nest.
  - A Victrix TT owner found 41-44 locked the boiler panel and gave no firing.
- [COMMUNITY] tado V3+ on a Victrix Tera 24 Plus: the boiler "did not power on" when wired to the bus terminals — [tado community, via search snippet](https://community.tado.com/en-gb/discussion/14134/immergas-victrix-tera-24-plus) (page now redirects; not read)
- [COMMUNITY] An Italian forum (fibra.click, via search snippet) reports a Victrix Tera 24 **V2** working with tado after setting **P7 = 2**. A second user with the same boiler could not confirm it — [forum.fibra.click](https://forum.fibra.click/d/62405-termostato-opentherm/13) (403 when fetched)
- [COMMUNITY] A Fantini C805OT OpenTherm chrono on a Victrix Tera TT 32 kept the boiler heating even with the chrono off — [plcforum Netatmo thread](https://www.plcforum.it/f/topic/304469-victrix-tera-24-netatmo-modulante-opentherm/)
- Immergas own controls listed as compatible with Victrix Tera: CAR V2 (wired and wireless), Mini CRD (modulating chrono, sold as compatible with Victrix TT/Exa/Tera/Omnia/Zeus), Smartech Plus (BLE thermostat + wired gateway; an HA user says the gateway talks OT to the boiler), and the Dominus Wi-Fi module — [climando Mini CRD listing](https://www.climando.it/en/products/termostato-cronotermostato-immergas-modulante-compatto-mini-crd-3020167-per-victrix-ttexateraomniazeus-mythos-hp); [HA thread p.1](https://community.home-assistant.io/t/immergas-victrix-integration-with-ha/395191)
- Immergas is reportedly not an OpenTherm Association member — [plcforum Nest thread](https://www.plcforum.it/f/topic/304253-immergas-vitrix-tera-nest-heat-link/); [domoticaforum, via search snippet](https://www.domoticaforum.eu/viewtopic.php?p=78190)

### Inferences
- **Terminal labelling**: the Tera manual calls the pair "44/40 and 41". Terminal 44 and 40 appear to be the same node (or the X40 jumper bridges them), and 41 is GND/−. Community reports saying "40-41" and "44-41" probably mean the same physical pair on this model **[VERIFY with a meter]**. The bus is polarity-sensitive per the manual (CAR V2 "rispettando la polarità", respecting polarity). A standard OT master is polarity-free on the receive side, but its slave-side detection is not, so check polarity.
- **Recommended bring-up order** for the user's own OT master on this boiler:
  1. Read d1.9 or the rating plate to tell V1 from V2.
  2. Measure the open-circuit voltage on 44/41 with X40 removed. About 15–18 V means IMG BUS/OT-like; about 24–35 V means CAR-BUS, and OT will not work.
  3. On V2, set P7 = 2 and power-cycle.
  4. Start with ID0 (CH enable + DHW enable), ID1 and ID57, and send them continuously (at least every second).
  5. Then try ID56 and ID14.
- **Expected quirks [VERIFY]**:
  - With P7 = 2, the manual says the DHW setpoint and max CH setpoint stay on the boiler panel. A master's **ID56 (DHW set) and ID57 (max CH set) writes may be ignored or read-only**, so test them.
  - Because the boiler may heat to ID57 rather than ID1 (as on some Eolo models), mirror ID1 into ID57.
  - When CH is off, also send ID1 = 10 °C as a safety measure.
  - ID17 may exceed 100 % during DHW (it scales to P0).
  - Error codes (ID5 / OEM fault) may not map to Immergas codes (Nest could not show errors).
  - The 0xCA status low byte used by original Immergas units should be tried if the boiler does not respond.
- **Comm-loss failsafe**: on loss of the OT link the boiler raises E31. After a power cycle without the remote it **refuses CH (DHW only)**, so a failed master leaves the house unheated. A fallback is to restore X40 (on/off mode) or to fit a relay that closes 40-41 when the master dies. Firmware should treat the bus as critical and keep the 1 s message cadence; OT spec comm loss is about 60 s.
- Because the on/off thermostat and OT share the same terminals, the user's thermostat can also run **dry-contact mode** (X40 removed, volt-free contact across 44/40-41) as a guaranteed fallback. In that mode the boiler uses its own flow setpoint or outdoor curve.

### Gaps
- No primary source lists exactly which OT Data-IDs the Tera answers (e.g. ID3, ID5, ID25, ID26, ID28 return temperature, ID56/57 write acceptance, ID15/ID48/49 bounds). No OTGW log for a Tera was found. The info menu shows return temperature (d0.8), so ID28 is plausible but unverified.
- OT protocol version (ID124/125) and member ID reported by the Tera are unknown.
- No official Immergas statement uses the word "OpenTherm" for the Tera; "commerciale" in P7 is the closest.
- Whether V1 firmware can be updated to gain P7 is unknown.
- Tado, Netatmo, Honeywell and DIYLESS official compatibility lists were not reachable or not checked (tado community redirects; the elektroda Exa/Nest thread timed out). Netatmo-on-Tera has no confirmed report.
- The Hungarian forums (prohardver, hoszakma) and immergas.hu were not searched within the call budget.

## 5. Summary of "verify on the real boiler" items
- V1 vs V2 (rating plate, d1.9). This decides whether P7 = 2 exists.
- Bus voltage on 44/41 with X40 removed (≤ 18 V = IMG BUS/OT-like; 24–35 V = CAR-BUS, not OT).
- Polarity, and whether "40" and "44" are the same node.
- Whether ID1 alone controls flow or ID57 must match it; whether ID56/ID57 writes are accepted with P7 = 2.
- ID17 scaling (CH max ≈ P2 ≈ 83–85 %?), ID28 return temperature, ID5 fault mapping.
- t4/t5 actual values (×10 s?) and pump post-run time.
- Behaviour after OT loss (E31) with and without a power cycle.
- Interaction of a fitted outdoor sensor with the OT setpoint (better not to fit the boiler sensor).
- Tank: NTC on 36–37 with R8 removed. The DHW setpoint + A4 offset defines the tank flow temperature. Anti-legionella must come from the master.
