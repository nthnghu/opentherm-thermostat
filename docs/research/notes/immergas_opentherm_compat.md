# Immergas Victrix Tera 24 Plus (V2): controlling it from a third-party OpenTherm master (ESP32 + DIYLESS shield)

Research date: 2026-11-15. This file complements `victrix_tera_24_plus.md` in the same folder, which covers the manual-level facts: P7, terminals, the E31 text and the parameter table. That material is only summarized here.

Evidence tags used throughout:
- **[CODE]**: read directly in source code that was cloned during this research.
- **[MANUAL]**: an official Immergas document.
- **[COMMUNITY]**: a forum or GitHub user report.
- **[UNKNOWN]**: not found.

---

## Q1. What does OTGateway's "Fix for Immergas boilers" do, and what do other projects do for Immergas?

### Takeaway
OTGateway's Immergas fix changes **one byte and nothing else**. The master sends ID0 (Read-Data, master status) with the **low byte set to 0xCA** instead of 0x00. The fix does not mirror ID1 into ID57; that is a separate, optional OTGateway setting. The original discoverer says the 0xCA status frame "unlocks" the boiler: without it, writes to ID1, ID56 and ID57 are acknowledged but have no effect. The only other Immergas-specific workaround found in code is in SAT (Smart Autotune Thermostat, a Home Assistant integration). SAT writes the OpenTherm transparent slave parameter (TSP) number 12 with min(max-modulation, 80) when it drives an OTGW. This is not proven on real hardware.

### Cited Findings
- **[CODE] OTGateway** (master branch, cloned at commit f9a814c, 2026-11-11), `src/OpenThermTask.h`, around lines 198–225:
  ```cpp
  // Set boiler status LB
  // Some boilers require this, although this is against protocol
  uint8_t statusLb = 0;
  // Immergas fix
  // https://arduino.ru/forum/programmirovanie/termostat-opentherm-na-esp8266?page=15#comment-649392
  if (settings.opentherm.options.immergasFix) { statusLb = 0xCA; }
  ... this->instance->setBoilerStatus(heating, dhw, cooling, nativeOTC, ch2, summerWinterMode, dhwBlocking, statusLb);
  ```
  — [OTGateway OpenThermTask.h](https://github.com/Laxilef/OTGateway/blob/master/src/OpenThermTask.h)
- **[CODE]** The frame is built in Laxilef's fork of the ihormelnyk library (branch `esp32_timer`, which OTGateway's `platformio.ini` uses). `buildSetBoilerStatusRequest()` puts CH enable (bit 0), DHW enable (bit 1), cooling (bit 2), OTC (bit 3), CH2 (bit 4), summer/winter (bit 5) and DHW blocking (bit 6) into the HB, shifts left 8 bits, then ORs in `lb`. The message type is READ_DATA on ID0. With CH and DHW on, the request data is therefore **0x03CA**. — [Laxilef/opentherm_library esp32_timer OpenTherm.cpp](https://github.com/Laxilef/opentherm_library/blob/esp32_timer/src/OpenTherm.cpp)
- **[CODE]** ID57 mirroring is a separate OTGateway option, `maxTempSyncWithTargetTemp` (UI label "Sync max heating temp with target temp"). When it is enabled, OTGateway writes `MaxTSet` (ID57) with the same value just before every `TSet` (ID1) write. It is **not** part of the Immergas fix. — [OTGateway OpenThermTask.h ~L1325–1345](https://github.com/Laxilef/OTGateway/blob/master/src/OpenThermTask.h); [en.json locale](https://github.com/Laxilef/OTGateway/blob/master/src_data/locales/en.json)
- **[COMMUNITY] Origin of 0xCA** (arduino.ru, user dex99). His boiler is an Immergas **EOLO STAR**. He reached his findings experimentally; they are not a published spec.
  - 2022-02-10: ImgBus is "тот же OpenTherm" (the same OpenTherm) with a quirk: "в первом запросе Status (ID0) в младшем байте установить фиксированное значение - 0xCA" (in the first Status (ID0) request, set the low byte to the fixed value 0xCA).
  - 2022-03-28 (comment 649392), the full sequence:
    1. Read (message type 0, not a write) ID0 with data HB=0x03, LB=0xCA.
    2. Read ID48 and ID49.
    3. Write ID1 and ID56.
    4. Write ID57. The heating temperature is written to **both ID1 and ID57**.
  - "Без первого шага запись в эти регистры ничего не меняет в работе котла" (without the first step, writes to these registers change nothing in how the boiler works).
  - The Immergas room unit spaces commands about 1 s apart; he found that ≤100 ms also works.
  - Source: [arduino.ru thread p.15](https://arduino.ru/forum/programmirovanie/termostat-opentherm-na-esp8266?page=15)
- **[CODE] SAT** (Alexwijn/SAT, `custom_components/sat/mqtt/opentherm.py` L206–208). When the manufacturer is Immergas, it sends `TP=11:12={min(value, 80)}` to the OTGW before every `MM=` (max relative modulation) command. The feature came in commit afc6c12 (2024-03-02), "Add support for manufacturers and also send a custom command when Immergas has been detected for Relative Modulation". `manufacturer.py` maps Immergas to **MemberID 27**. — [SAT repo](https://github.com/Alexwijn/SAT/blob/main/custom_components/sat/mqtt/opentherm.py)
- **[CODE/DOC] OTGW-firmware** (rvdbreemen, dev branch, cloned 2026-11-15) ports the SAT behaviour:
  - ADR-207 describes TP=11:12 as "transparent slave parameter write of the maximum modulation level, `min(MM, 80)`". Data-ID 11 is OpenTherm's TSP index/value message; index 12 is the Immergas parameter.
  - The changelog says: "No real Immergas boiler was tested… covered by host tests and by watching the commands on the bench".
  - **MemberID 27 is shared by Immergas and Sime.** Auto-detection was therefore sending the Immergas TSP write to Sime boilers, and the firmware now requires the user to select Immergas explicitly (TASK-1537).
  - The SAT comparison doc notes that "What [TSP 12] does in the boiler is not shown in either code".
  - Sources: [OTGW-firmware v2.0.0-beta.38 release](https://github.com/rvdbreemen/OTGW-firmware/releases/tag/v2.0.0-beta.38); [OTGW-firmware CHANGELOG (dev)](https://github.com/rvdbreemen/OTGW-firmware/blob/dev/CHANGELOG.md)
- **[COMMUNITY] OTGateway compatibility wiki.** "IMG-BUS from Immergas is an analogue of the OpenTherm protocol with minor changes." Listed models:
  - Nike Mythos 24: "Fix for Immergas boilers".
  - Victrix Zeus 25: Immergas fix; DHW flow-rate sensor not supported.
  - Eolo Mythos 24 4 R: Immergas fix + "CH2 always enabled".
  - Source: [OTGateway Compatibility wiki](https://github.com/Laxilef/OTGateway/wiki/Compatibility)
- **[COMMUNITY] Victrix Zeus 25 with OTGateway**: ID19 (DHW flow rate) returns INVALID (`Request: 80130000 Response: 70130000`). Per Immergas, via the user, "ID19 is active only in models with direct flow heating without a hot water storage tank". The same user relays "non-public information from their developer" that Immergas "is not in the OpenTherm association, but its extension modules are based on OpenTherm protocols". — [OTGateway #92](https://github.com/Laxilef/OTGateway/issues/92)
- **[COMMUNITY] OT Thing gateway** (Phunkafizer). The v2.9 release notes (2025-11-12) include "Configuration of statusrequest for immergas boiler". This is presumably the same configurable ID0 low byte, but that was not verified in its code. A Victrix Tera V2 35 Plus owner asked about compatibility; the answer was "you only can try". — [HA community OT Thing thread p.3](https://community.home-assistant.io/t/ot-thing-an-opentherm-wifi-gateway-with-integrated-ot-master-slave/824667?page=3)
- **[COMMUNITY] jotuera hub** (ESPHome, marked experimental):
  - It sends ID0 with LB 0xCA, then ID48 and ID49, then writes ID1, ID56 and ID57.
  - It writes ID57 = CH setpoint every cycle and ID14 every cycle.
  - When CH is off it sends ID1 = 10 °C.
  - It notes that "Some Immergas boilers (EOLO Star, Mini EOLO) acknowledge Data-ID 1 and heat to Data-ID 57".
  - Source: [jotuera opentherm-hub.md](https://raw.githubusercontent.com/jotuera/immergas-modbus-bms-nasa-opentherm-hub/main/docs/opentherm-hub.md) (already summarised in victrix_tera_24_plus.md §4)
- **[COMMUNITY] SmartTherm (Evgen2)** also has an "Immergas fix". A user with an Eolo Mythos 24 4R saw mostly INVALID responses, with some responses carrying a different Data-ID from the request. The maintainer suggested "Immergas fix поломался в ветке 0.8.5" (the Immergas fix may have broken in the 0.8.5 branch). — [SmartTherm #5](https://github.com/Evgen2/SmartTherm/issues/5)
- No Immergas-specific workaround was found in ESPHome core's `opentherm` component or in the ihormelnyk upstream library. A GitHub issue search for "immergas opentherm" returned no ESPHome-core or ihormelnyk Immergas issues. — [GitHub search results (API)](https://github.com/search?q=immergas+opentherm&type=issues)

### Inferences
- For our own firmware the minimum is to send **ID0 as READ_DATA with data = (master flags << 8) | 0xCA**, on every status poll and not only the first. OTGateway sends it on every cycle and that works on Tera 28, Tera 24 Plus and Exa (Q2). This breaks the OT spec, which wants LB = 0 in the request. Make it a config flag.
- Write ID1 = flow setpoint every cycle **and also write ID57 = the same value**, as dex99's sequence and the jotuera hub do. A boiler that "heats to ID57" then still follows us. One caution: with P7=2 the V2 manual says the max CH setpoint stays on the boiler panel, so the ID57 write may be ignored. That is harmless.
- Optional, untested: Immergas TSP 12 (write ID11 with HB = 12, LB = value) as max modulation, capped at 80. Only try it after reading the current value (ID11 read with HB=12), because its meaning is unconfirmed. **Do not write it blindly.**
- Expect INVALID or "unknown data-ID" responses for some optional IDs (19, possibly 18/26/28/33). Treat them as unsupported and not as link failures.

### Gaps
- What 0xCA means to the boiler, and whether it is needed on every frame or only the first, is undocumented. dex99 says "first request"; all the code bases send it every time.
- The semantics of Immergas TSP #12 are unverified, and no OTGW log from an Immergas boiler was found.
- The arduino.ru thread does not say whether the Mini Eolo user succeeded with 0xCA.

---

## Q2. Field reports by model and controller

### Takeaway
DIY OpenTherm masters running the Immergas fix (0xCA) now have **confirmed success on a Victrix Tera 24 Plus** (Jan 2026), on a Victrix Tera 28, Victrix Exa 24 X 1 ErP, Victrix Zeus 25, Victrix Omnia 25, Nike Mythos 24, Eolo Mythos 24 4R and Maior Eolo. There is **one failure on a Tera 24 Plus** (Oct 2026, OTGateway #284, total TIMEOUT) that is unresolved and was probably a wiring, GPIO or bus problem. None of the success reports says V1 or V2, and none mentions P7. Commercial thermostats have mixed results: Nest Heat Link works with modulation on a Tera 24; tado on a Tera 24 V2 works with P7=2 per one report, and failed in another (version unknown); a Fantini OT chrono failed on a Tera TT 32; an evohome R8810 behaved oddly on a Victrix 32 ErP. Boilers whose remote bus is CAR-BUS at about 28 V (e.g. Victrix TT) do not work.

### Cited Findings
**DIY / open-source masters**
- **[COMMUNITY] SUCCESS, Victrix Tera 24 Plus** (gorenoj, 2026-01-26). Hardware: Wemos D1 mini (ESP8266) + **DIYLESS ESP8266 Thermostat Shield**, running OTGateway. "I can confirm compatibility with Immergas Victrix Tera 24 plus… I checked 'Fix for immergas boilers'." No V1/V2 or P7 information was given. — [OTGateway #3 comment](https://github.com/Laxilef/OTGateway/issues/3#issuecomment-3801355953)
- **[COMMUNITY] SUCCESS, Victrix Tera 28** (tomashejatko, 2025-10-05). The user removed the factory red/black wire link (the X40 jumper) and connected a DIYLESS shield to **pins 40 and 41**, with the Immergas fix on. "Everything seems to be working great except DHW flow rate sensor". A follow-up user (2026-01-27) could not get it working. — [OTGateway #3 comment](https://github.com/Laxilef/OTGateway/issues/3#issuecomment-3369153481)
- **[COMMUNITY] SUCCESS, Victrix Exa 24 X 1 ErP** (2026-01-14). DIYLESS Master OpenTherm Shield + NodeMCU ESP8266, stock settings + Immergas fix. "Pressure, exhaust temp not work, main functions work". — [OTGateway #3 comment](https://github.com/Laxilef/OTGateway/issues/3#issuecomment-3747914846)
- **[COMMUNITY] SUCCESS, Victrix Zeus 25** (2024-11-03). **DIYLESS Master OpenTherm Shield** + D1 mini, Immergas fix on. "Key functions are working fine"; DHW flow rate does not work. — [OTGateway #3 comment](https://github.com/Laxilef/OTGateway/issues/3#issuecomment-2453361792)
- **[COMMUNITY] SUCCESS, Maior Eolo.** Terminals 41/44 after removing link S20. The boiler reports as "Baxi", Member ID 27, CH limits 35–85 °C, DHW limits 30–60 °C. Return temperature, DHW flow and exhaust temperature are not reported. — [OTGateway #3 comment](https://github.com/Laxilef/OTGateway/issues/3#issuecomment-4278712153)
- **[COMMUNITY] SUCCESS, Victrix Omnia 25** with ESPHome (esp-opentherm) + SAT: "works as expected"; member ID 27. — [SAT discussion #21 (via search summary)](https://github.com/Alexwijn/SAT/discussions/21)
- **[COMMUNITY] Works but cycles, Victrix Zeus 25 / Victrix Omnia** with the DIYLESS OpenTherm Thermostat 3. Comms work, but at low setpoints the boiler short-cycles. On the Omnia the burner ignites at about 38 % for about 20 s, cuts out at setpoint +5 K, then waits about 3 min. The Omnia user "cannot really modulate under 40 degrees… for any reasonable amount of time". Workarounds: raise t0 (minimum CH setpoint) to 30–35 °C, and lower the ignition fan speed S2 (needs a recalibration). — [diyless thermostat3 #34](https://github.com/diyless/opentherm-thermostat3/issues/34); [#31](https://github.com/diyless/opentherm-thermostat3/issues/31#issuecomment-3744691452)
- **[COMMUNITY] Partial / timing, "Immergas Vitrix Extra 12 Plus"** (probably a Victrix Extra) with DIYLESS Thermostat 3, 2026-08-21:
  - 17.6 % of requests got no answer (`B0000`), worst on ID25 (31/122), ID0 (30/122) and ID17 (26/122). The rest answered normally.
  - Writes ACKed: `T10010a00` → `Bd0010a00` (ID1 = 10 °C).
  - ID18 (pressure) answered 0x0199 ≈ 1.6 bar; ID25 answered 0x177f ≈ 23.5 °C.
  - The boiler display showed the remote/OT symbol.
  - DIYLESS's explanation: the boiler's reply is near or outside the 800 ms window.
  - Source: [diyless thermostat3 #32 comment](https://github.com/diyless/opentherm-thermostat3/issues/32#issuecomment-5373052226)
- **[COMMUNITY] FAILURE, Victrix Tera 24 Plus** ("original version, approximately 2021"). ESP32 + DIYLESS shield, OTGateway 1.6.0 with the Immergas fix; ESPHome failed too.
  - Symptom: every request returned TIMEOUT.
  - The CAR V2 info menu shows "VER PI" (IMG-BUS), and the CAR V2 is code 3.021623 (the wireless one).
  - Not done: bus voltage was never measured, the GPIO swap was never tried, and P7 was never mentioned. Open/stale.
  - A Victrix TT owner in the same thread measured **28 V** open-circuit (CAR-BUS). He cites Immergas doc 1.038958: CAR-Bus 24–35 V DC / 10 mA versus IMG-Bus ≤18 V DC / 23 mA.
  - Source: [OTGateway #284](https://github.com/Laxilef/OTGateway/issues/284)
- **[COMMUNITY] FAILURE, Eolo Mythos 24 4R** (a "listed compatible" model) with a SmartKot PC817 shield and OTGateway 1.6.0. 16 V on 40–41, never connects. The maintainer added the model to the list from a user report and has not tested it himself. — [OTGateway #268](https://github.com/Laxilef/OTGateway/issues/268)

**Commercial thermostats**
- **[COMMUNITY] Nest Heat Link on a Victrix Tera 24**, 2022: OT on 40–41. Works "alla perfezione" (perfectly) with modulation, flow around 45 °C. Only the DHW maximum can be set, and boiler errors do not show. — [plcforum Nest thread](https://www.plcforum.it/f/topic/304253-immergas-vitrix-tera-nest-heat-link/)
- **[COMMUNITY] tado.** One Italian report says a Victrix Tera 24 **V2** with tado works after **P7 = 2**. A Tera 24 Plus (version unknown) + tado V3+ "did not power on". Neither report was read in full; both come from search snippets. — [forum.fibra.click](https://forum.fibra.click/d/62405-termostato-opentherm/13); [tado community](https://community.tado.com/en-gb/discussion/14134/immergas-victrix-tera-24-plus)
- **[COMMUNITY] Netatmo modulating on a Victrix Tera 24** (2022): the question was asked but no result was reported. Immergas documentation "inesistente, da nessuna parte si parla di OpenTherm" (non-existent; OpenTherm is not mentioned anywhere). A Tera 28 + CAR V2 owner notes the boiler has only one remote pair, so it is one controller or the other. — [plcforum Netatmo/Tera 24](https://www.plcforum.it/f/topic/304469-victrix-tera-24-netatmo-modulante-opentherm/); [plcforum Netatmo/Tera 28](https://www.plcforum.it/f/topic/304571-netatmo-immergas-victrix-tera-28-car-v2/)
- **[COMMUNITY] Fantini C805OT** OpenTherm chrono on a Victrix Tera TT 32: the boiler stays in heating even with the chrono off. The user reverted to on/off. — [plcforum Netatmo thread](https://www.plcforum.it/f/topic/304469-victrix-tera-24-netatmo-modulante-opentherm/)
- **[COMMUNITY] Honeywell evohome R8810** on 44/41 of a Victrix 32 ErP: DHW stuck at 30 °C, CH at 85 °C. — [HA thread p.1](https://community.home-assistant.io/t/immergas-victrix-integration-with-ha/395191)
- **[COMMUNITY] Generic ESP32 + OT adapter** on 41/44 (model unstated): "solved… able to communicate". JoTu (2026-11-07) gives the per-model gotchas:
  - TERA: P7 = 2.
  - ZEUS Superior / HERCULES: **M3 · P33 = ON** keeps CH running if the remote is lost.
  - "Up to ~18 V DC is IMG BUS, which works with OpenTherm"; "~24–35 V DC is the older CAR-BUS, which no OpenTherm device can talk to".
  - Without a remote, the boiler "waits for the remote (E31) and doesn't heat".
  - Source: [HA thread p.2](https://community.home-assistant.io/t/immergas-victrix-integration-with-ha/395191?page=2)
- **[COMMUNITY]** A Spanish forum post claims the Tera 28 is not OT-compatible (on/off only); the version and date are unclear. — [nergiza.com](https://nergiza.com/foro/threads/caldera-vitrix-tera-28-kw-y-termostato-opentherm-modulante.8663/) (search snippet only)

### Inferences
- The success count (Tera 24 Plus, Tera 28, Exa, Zeus, Omnia) against the single unresolved Tera 24 Plus TIMEOUT suggests **the Tera family speaks IMG-BUS that OT masters can drive**. The TIMEOUT case is more likely wiring, GPIO or bus-voltage trouble than a protocol block. Bring-up should begin with a voltmeter (expect about 15–18 V idle) and an RX-activity check.
- **The DIYLESS shield is the most-reported working hardware on Immergas**: Zeus 25 and Exa with the Master Shield, Tera 24 Plus with the ESP8266 Thermostat Shield, Tera 28 with a DIYLESS board.
- On low-load radiator circuits, plan for the Omnia/Zeus behaviour: a high ignition burst, then a +5 K overshoot cut-out and about 3 min anti-cycle. Set the boiler's own minimum CH setpoint (t0) and anti-cycling (t3/t4-type parameters) sensibly, and keep the master's setpoint above roughly 35 °C while firing.

### Gaps
- None of the success reports states V1/V2 or the P7 value. Whether a V1 Tera 24 Plus (where P7 is "not available") works was not established; the 2026-01 success could be either version.
- No Honeywell T6/Lyric, Bosch, Ariston or Netatmo OTH success on a Tera was found.
- The elektroda.pl Exa/Tera thread (topic3843909) and the "[Solved] Zeus Victrix + PT52" thread (topic3868351) timed out and were not read. The search summary says a Polish poster quotes an Immergas written reply: "Victrix TERA V2 EU posiada wbudowaną automatykę OpenTherm" (has built-in OpenTherm control), activated via P7. This was not verified first-hand.

---

## Q3. Official Immergas statements, interfaces and adapter kits

### Takeaway
No official Immergas document found uses the word "OpenTherm" for the Tera. The closest official wording is the V2 manual's **P7 = 2, "comando remoto commerciale"** (commercial remote control) on 44/41. The 2025 Victrix Tera V2 brochure lists only Immergas controls: CAR V2 (wired/wireless), CRD Plus, Mini CRD, Smartech Plus, CRONO 7 (on/off) and the GSM/landline telephone commands. **No Immergas OpenTherm interface kit or 0–10 V module exists for this boiler.** Immergas is reportedly not an OpenTherm Association member.

### Cited Findings
- **[MANUAL] IT V2 (24 Plus)**: P7 "Comando remoto". 0 = IMG BUS for Immergas remotes (e.g. CAR v2) on 44/41; 1 = not available on this model; 2 = "comando remoto commerciale ai morsetti 44/41". With 2, the DHW and max-CH-flow settings stay on the boiler panel. Default 0. — [M2 V2 manual](https://gimli.freetls.fastly.net/tavolla/Products/Attachments/40503/inst.pdf)
- **[MANUAL] HU V2 (Victrix Tera V2 28/32/38 EU, doc 1.047820HUN, on immergas.hu)**: same parameter, worded "P7 Távvezérlő… 0: IMG BUS… 2: Válassza ezt az üzemmódot, ha kereskedelmi távirányítót csatlakoztat a 44/41-es kapcsokhoz (ezzel a beállítással a használati melegvíz és a maximális fűtési előremenő beállításai továbbra is elérhetők maradnak a kazán paneljén)". In English: choose this mode when connecting a commercial remote to 44/41; with it, the DHW and max CH flow settings remain on the boiler panel. Range 0–2, default 0. — [immergas.hu Victrix Tera V2 28-32-38 HU](https://immergas.hu/sites/default/files/documents/Victrix%20Tera%20V2%2028-32-38_HUN.pdf)
- **[MANUAL] HU original Plus manuals**:
  - The immergas.hu "Victrix Tera 24 Plus / 35 Plus" manual (1.040296HUN) has "P7 – Nem elérhető ennél a modellnél" (not available on this model). — [immergas.hu Victrix Tera 24/35 Plus HU](https://immergas.hu/sites/default/files/termekek/kazan/dokumentumok/Victrix-Tera%2024%2035%20Plus_HU.pdf)
  - An older Tera 24 Plus HU manual (1.040295, rev. 02/17) says the same. — [szerelvenybolt doc4.pdf](https://files.szerelvenybolt.hu/pdf/doc4.pdf)
  - **No HU-language V2 24 Plus manual was found on immergas.hu**, which still links the original-version Plus manual.
- **[MANUAL] Brochure "VICTRIX TERA V2"** (immergas.com, InDesign metadata 2025-05-30). It says "VICTRIX TERA V2 può essere collegata a CARV2, CARV2 WIRELESS, CRD PLUS o MINI CRD" (can be connected to CARV2, CARV2 WIRELESS, CRD PLUS or MINI CRD), with Smartech Plus (BLE + Wi-Fi gateway, 3.030909) also available. The control list:

  | Control | Code(s) |
  |---|---|
  | CARV2 | 3.021395; wireless 3.021623 |
  | CRD PLUS | 3.032645 |
  | MINI CRD | 3.020167; "solo per impianti a 1 zona e senza… sonda esterna" (only for single-zone systems, without an outdoor sensor) |
  | CRONO 7 (on/off) | 3.021622; wireless 3.021624 |
  | Comando telefonico GSM | 3.017182 |
  | Comando telefonico | 3.013305 |
  | Multizone board (needs ≥1 CARV2) | 3.028444 |

  The text contains no "OpenTherm", no "0-10" and no third-party wording (searched in the extracted text). — [Immergas VICTRIX TERA V2 brochure](https://www.immergas.com/media/Prodotto/63d279c7bc8e3c6a1f42b67e/VICTRIX%20TERA%20V2)
- **[MANUAL]** The V2 Plus manual text contains no 0–10 V input. The only room-control inputs are the on/off contact or the remote on 44/40–41 with X40 removed, plus the outdoor sensor. — [M2 V2 manual](https://gimli.freetls.fastly.net/tavolla/Products/Attachments/40503/inst.pdf)
- **[COMMUNITY]** Immergas support told a Nest user the Tera 28 supports "Open Therm modificato" (modified OpenTherm), "al 99 %" (99 % sure), and that errors may not show. — [plcforum Nest thread](https://www.plcforum.it/f/topic/304253-immergas-vitrix-tera-nest-heat-link/)
- **[COMMUNITY]** "Immergas is not in the OpenTherm association, but its extension modules are based on OpenTherm protocols" (relayed from an Immergas developer). — [OTGateway #92](https://github.com/Laxilef/OTGateway/issues/92)
- **[COMMUNITY]** The Dominus Wi-Fi module has a third-party HA integration ([jotuera/immergas-dominus](https://github.com/jotuera/immergas-dominus/issues/2)) with mixed compatibility, e.g. a Zeus 25 ErP with no replies. A Smartech Plus HA integration exists (PeppeWH, 2026-05). Neither is an OpenTherm interface. — [HA thread p.2](https://community.home-assistant.io/t/immergas-victrix-integration-with-ha/395191?page=2)

### Inferences
- With P7=2 Immergas *officially provides* for a non-Immergas remote on 44/41. Since the boiler's bus is IMG-BUS, that remote is necessarily OpenTherm-like, which makes P7=2 the officially sanctioned route. Immergas simply does not say "OpenTherm".
- A **Hungarian installer** should be able to set P7 (installer menu) on a V2 unit. For a 2026 installation, insist on the V2 model, and get the code on the invoice or rating plate.

### Gaps
- No official Immergas IT/UK/HU statement or FAQ on OpenTherm was found; immergas.hu was searched only through its PDFs.
- Whether the new **CRD PLUS (3.032645)** speaks plain OT is unknown.
- No "kit interfaccia OpenTherm" from Immergas was found; the evidence suggests none exists.

---

## Q4. Hungarian sources

### Takeaway
No Hungarian-language forum experience (prohardver, hoszakma, mastersfórum, Facebook) with a Victrix Tera on OpenTherm was found in these searches. The only Hungarian primary sources are the immergas.hu manuals. They confirm that the HU-published Tera 24 Plus manual is the **original version (P7 not available)** and that the HU V2 combi manual has P7=2 for a commercial remote. The Hungarian manuals call the Immergas remote **"Amico V2" (= CAR V2)**.

### Cited Findings
- immergas.hu Tera 24/35 Plus HU manual (1.040296HUN). Terminology: "Amico V2 (CAR V2) távvezérlő" connects to "a 44/40-es és 41-es" terminals; P7 not available on this model. Error list entries:
  - "31 Távvezérlő jel hiba" (remote signal error)
  - "51 Vezetéknélküli Amico kommunikációs hiba" (wireless Amico communication error); control then falls back to the boiler panel only.
  - Source: [immergas.hu Plus HU](https://immergas.hu/sites/default/files/termekek/kazan/dokumentumok/Victrix-Tera%2024%2035%20Plus_HU.pdf)
- The older HU Tera 24 Plus manual also lists "36 Hiba a kazán IMG Bus kommunikációjában" (fault in the boiler's IMG Bus communication), related to supply voltage. — [szerelvenybolt doc4.pdf](https://files.szerelvenybolt.hu/pdf/doc4.pdf)
- HU V2 28/32/38 manual: "31 Távvezérlő jel elvesztése – Ez akkor következhet be, ha a készüléket nem kompatibilis távvezérlőhöz csatlakoztatja, vagy ha a távvezérlő és a berendezés között megszűnik a kommunikáció" (31 = loss of remote signal; happens with an incompatible remote or when communication drops). It also has "d 1.9 Váltakozva jeleníti meg a biztonsági szoftver és az üzemi szoftver verziószámát" (d1.9 alternately shows the safety and functional software versions). — [immergas.hu V2 HU](https://immergas.hu/sites/default/files/documents/Victrix%20Tera%20V2%2028-32-38_HUN.pdf)
- A Hungarian-language search returned only Italian, Polish and Spanish threads. — (searches "Victrix Tera OpenTherm termosztát", "Immergas OpenTherm kompatibilis kazán Victrix Tera netatmo tado tapasztalat")

### Inferences
- Hungarian installers will know the remote as "Amico V2". Ask them to set **"P7 Távvezérlő = 2"** on a V2.

### Gaps
- prohardver.hu, hoszakma.hu and Facebook groups were not indexed by the search tool for this topic. Nothing was found, but this is not proof of absence.
- Whether immergas.hu currently sells the V2 24 Plus (versus V1 stock) is unknown.

---

## Q5. Telling V2 from the original, and software version

### Takeaway
On the rating plate and invoice, the name reads "VICTRIX TERA **V2** 24 PLUS (EU)" on the new generation. The functional test is decisive: **open the installer menu and see whether P7 offers 0/2**. V1 shows P7 as unavailable. The info menu item **d1.9** shows the safety and functional software versions alternately, but no mapping of software version to V1/V2 was found.

### Cited Findings
- d1.9 "Visualizza alternativamente la versione software di sicurezza e la versione software funzionale" (alternately shows the safety and functional software versions). — [M2 V2 manual](https://gimli.freetls.fastly.net/tavolla/Products/Attachments/40503/inst.pdf); HU equivalent in [immergas.hu V2 HU](https://immergas.hu/sites/default/files/documents/Victrix%20Tera%20V2%2028-32-38_HUN.pdf)
- Product codes: original VICTRIX TERA 24 PLUS control-panel sheet code **3.027373**; Victrix Tera 24 V2 (combi) **3.032927**; the V2 24 PLUS EU code 3.032933 is per victrix_tera_24_plus.md. — [Immergas PC_3.027373.pdf (search result title)](https://webdav.immergas.com/webdav.usr/ro-guest/service.ba1811242196b1998c8d6ce2196f2a12/PdfImmergas/Pannelli%20Di%20Controllo/PC_3.027373.pdf); [Tera 24 V2 product page](https://www.immergas.com/prodotto/3.032927/victrix_tera_24_v2/)
- A CAR V2's info menu "VER PI" indicates IMG-BUS and "VER PC" indicates CAR-BUS. This identifies the bus, not V1/V2. — [OTGateway #284](https://github.com/Laxilef/OTGateway/issues/284)

### Gaps
- Whether a V1 board can be updated to V2 firmware (and gain P7) is unknown.

---

## Q6. Runtime behaviour with an OT/third-party master

### Takeaway
- **Modulation**: when the boiler accepts the master, it modulates on the flow setpoint. Evidence is the Nest result and the DIY reports showing CH setpoint, flame and modulation.
- **Reported data**: ID17 (modulation), ID25 (flow) and ID18 (pressure, on some models) are reported. ID19 is absent on storage/system boilers, and ID28 (return) and ID33 (exhaust) are missing on several models. The DHW setpoint and max CH setpoint may stay on the boiler panel with P7=2.
- **Comm loss**: E31. If the boiler is power-cycled without a remote, CH is disabled. The Tera has no documented "keep heating on remote loss" option, unlike Zeus Superior/Hercules (M3·P33).

### Cited Findings
- Nest on Tera 24: modulation icons, flow around 45 °C to hold 19 °C, only DHW max settable. — [plcforum Nest thread](https://www.plcforum.it/f/topic/304253-immergas-vitrix-tera-nest-heat-link/)
- Victrix Extra 12 Plus: ID25, ID18 and ID17 answered; ID1 write ACKed with 10 °C. — [diyless thermostat3 #32](https://github.com/diyless/opentherm-thermostat3/issues/32#issuecomment-5373052226)
- Maior Eolo: return, DHW flow and exhaust not reported; CH bounds 35–85 °C, DHW bounds 30–60 °C (ID48/49); OT version 0; MemberID 27. — [OTGateway #3](https://github.com/Laxilef/OTGateway/issues/3#issuecomment-4278712153)
- Zeus 25: ID19 INVALID, "active only in models with direct flow heating without a hot water storage tank". Our Tera **Plus** has a tank, so expect ID19 unsupported. — [OTGateway #92](https://github.com/Laxilef/OTGateway/issues/92)
- Exa 24: pressure and exhaust temperature not working. — [OTGateway #3](https://github.com/Laxilef/OTGateway/issues/3#issuecomment-3747914846)
- jotuera hub: ID17 "can exceed 100 % during DHW". — [jotuera opentherm-hub.md](https://raw.githubusercontent.com/jotuera/immergas-modbus-bms-nasa-opentherm-hub/main/docs/opentherm-hub.md)
- [MANUAL] E31: on comm loss, reset by power cycle. If no remote is detected at power-up, the boiler goes to local mode and "non è possibile attivare la funzione Riscaldamento" (Heating cannot be switched on). — [M1 original manual p.46](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf); same wording for V2 in [immergas.hu V2 HU](https://immergas.hu/sites/default/files/documents/Victrix%20Tera%20V2%2028-32-38_HUN.pdf)
- [COMMUNITY] ZEUS Superior / HERCULES: M3·P33 = ON keeps CH running if the remote is lost; Tera: P7=2. Without a remote the boiler waits (E31) and does not heat. — [HA thread p.2](https://community.home-assistant.io/t/immergas-victrix-integration-with-ha/395191?page=2)
- [COMMUNITY] Reply timing is sometimes near or beyond 800 ms (Extra 12 Plus, 17.6 % misses); OTGateway waits up to 1000 ms and retries 5 times before declaring a disconnect (about 6.3 s). — [diyless #32](https://github.com/diyless/opentherm-thermostat3/issues/32#issuecomment-5373052226); [OTGateway OpenThermTask.h](https://github.com/Laxilef/OTGateway/blob/master/src/OpenThermTask.h)
- [COMMUNITY] The Immergas room unit sends about 1 frame per second; dex99 found 100 ms spacing OK. — [arduino.ru p.15](https://arduino.ru/forum/programmirovanie/termostat-opentherm-na-esp8266?page=15)

### Inferences: code requirements for our firmware
1. ID0 READ with LB=0xCA on every cycle, HB = CH enable | DHW enable (0x03 when both). Configurable.
2. Write ID1 every cycle, at most about 1 s apart. Also write ID57 = ID1, and tolerate it being ignored or rejected.
3. When CH is not wanted, clear the CH bit **and** write ID1 = 10 °C. This guards against "keeps firing to the last setpoint", seen with the Fantini chrono and noted in jotuera.
4. Treat a single missed reply as normal. Use an RX timeout of about 1 s, at least 1–2 retries, and declare loss only after about 5 s or more. Never stop the master cadence: the boiler will raise E31.
5. Poll a minimal set (ID0, ID1 write, ID25, ID17, ID18) at high rate and the rest (ID3, ID5, ID28, ID48/49, ID56, ID125, ID127) slowly. Mark INVALID or UNKNOWN-ID responses as "unsupported", not as a link fault.
6. ID17 on DHW may read >100 %; clamp it for display.
7. Firmware watchdog/failsafe: if the ESP dies, the boiler raises E31, and after a power cycle it will not heat. Hardware fallback: a normally-closed relay contact across 44/40–41 that closes when the ESP is dead re-creates "X40 present", so the boiler runs on its own on/off logic. **[VERIFY]** the boiler accepts a switch from OT to a closed contact without a power cycle. The manual implies a power cycle is needed after E31.

### Gaps
- No Tera-specific OT trace exists (which IDs it answers, ID3 slave flags, ID125 OT version, ID5 fault mapping to Immergas codes).
- How long the Tera waits before E31 is undocumented; the OT spec says about 60 s, but community wording only says "waits".
- Whether ID14 (max relative modulation) is honoured or Immergas TSP #12 is needed is unknown.

---

## Q7. Fallbacks if OT does not work

### Takeaway
The guaranteed fallback is the **volt-free on/off contact on 44/40–41** (with X40 removed, or a relay across it). The boiler then modulates on its own fixed flow setpoint or outdoor-sensor curve. No 0–10 V input or Immergas OT or 0–10 V kit exists for this model. Other Immergas-ecosystem routes are the CAR V2 or Mini CRD (proprietary IMG-BUS), the Dominus Wi-Fi module, and the telephone-command module. None of them gives an open modulating interface.

### Cited Findings
- [MANUAL] On/off thermostat on 44/40 and 41 with X40 removed; the contact must be volt-free ("pulito"), or the board is damaged. — [M2 V2 manual](https://gimli.freetls.fastly.net/tavolla/Products/Attachments/40503/inst.pdf); [M1](https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf)
- [MANUAL] Accessory list: no 0–10 V or OpenTherm kit. Telephone commands (GSM 3.017182 and landline 3.013305) are "abbinabile a tutti i dispositivi di termoregolazione Immergas (ad eccezione del MINI CRD)" (compatible with all Immergas controls except the MINI CRD). — [VICTRIX TERA V2 brochure](https://www.immergas.com/media/Prodotto/63d279c7bc8e3c6a1f42b67e/VICTRIX%20TERA%20V2)
- [MANUAL] Outdoor sensor 3.014083 for sliding (weather-compensated) temperature. — [VICTRIX TERA V2 brochure](https://www.immergas.com/media/Prodotto/63d279c7bc8e3c6a1f42b67e/VICTRIX%20TERA%20V2)
- [COMMUNITY] The Dominus HA integration and the Smartech Plus integration are options outside OT. — [jotuera/immergas-dominus #2](https://github.com/jotuera/immergas-dominus/issues/2); [HA thread p.2](https://community.home-assistant.io/t/immergas-victrix-integration-with-ha/395191?page=2)

### Inferences
- Wire the ESP so that one relay can switch 44/40–41 between "OT master" and "dry contact closed or open". The thermostat then works as an on/off controller if OT fails, and it is also the failsafe.
- An outdoor sensor fitted to the boiler would make the boiler compute its own flow temperature in on/off mode. With an OT master, it is better to leave it off, or to verify how it interacts with ID1.

### Gaps
- Whether the Tera V2 accepts mode changes between OT and contact without a power cycle is unverified.
