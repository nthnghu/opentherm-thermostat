# Hydraulics, multi-zone demand aggregation and TRVs for a single condensing system boiler (2 radiator circuits + 1 mixed underfloor circuit + external DHW tank)

Scope note: most sources found are practitioner (Heat Geek, forums) and manufacturer literature; very few formal standards. Where a claim comes only from a forum it is flagged "anecdotal". The researcher's own engineering reasoning is kept in "Inferences". Tool-call budget limited the depth of fetches (several pages were seen only as search snippets).

## 1. Hydraulic separators / low-loss headers, alternatives, and automatic bypass valves

### Takeaway
A low-loss header (LLH) is only needed when there are several pumps whose flows must be decoupled (or the boiler pump can't push the system); if the boiler-side (primary) flow exceeds the system-side (secondary) flow, hot boiler flow short-circuits straight back into the boiler return and raises return temperature, cutting condensing. Automatic bypass valves have the same effect whenever they open. For a small domestic system with a modern 7 m-head modulating internal pump, direct connection (with a correctly set minimum-flow path) is usually preferred.

### Cited Findings
- LLHs are used for "hydraulic separation" between two or more circulating pumps so each runs at its own flow; in domestic systems they are usually fitted where the boiler's internal pump lacks power or pipework resistance is high — [Heat Geek: Low loss headers](https://www.heatgeek.com/low-loss-headers/)
- Underfloor heating needs roughly 3x the flow rate of radiators, which internal boiler pumps can struggle to supply — this is a common reason to add a separate UFH pump/separation — [Heat Geek: Low loss headers](https://www.heatgeek.com/low-loss-headers/)
- Most internal boiler pumps are now higher-head modulating pumps (~7 m head); Heat Geek advises checking heat-loss/flow calculations first because oversized estimates are common (i.e., an LLH may be unnecessary) — [Heat Geek](https://www.heatgeek.com/low-loss-headers/)
- With an LLH, mixing in the header causes "distortion": the boiler must run hotter than the emitters require, causing "a small % loss in efficiency with gas boilers" (no figure given); pumps either side can interfere, especially two modulating pumps ("erratic and oscillating/pulsing reaction") — [Heat Geek](https://www.heatgeek.com/low-loss-headers/)
- Design rules: header velocity below ~0.3 m/s (0.2 m/s also mentioned); single common primary/secondary connection, avoid multiple tappings that short-circuit return into flow; vertical headers separate air/dirt better — [Heat Geek](https://www.heatgeek.com/low-loss-headers/)
- An LLH keeps boiler return close to system return only when secondary flow > primary flow; if primary > secondary, flow in the header reverses, sending boiler supply straight back to the boiler return and raising return temperature; Viessmann instructions reportedly require heating-circuit flow to exceed boiler-circuit flow — [HeatingHelp forum: low loss header understanding](https://forum.heatinghelp.com/discussion/142026/low-loss-header-am-i-understanding-this-correctly) (practitioner forum)
- Amount of reverse flow through headers depends on relative resistance of the heat-source/header piping vs. mixing valve and inactive zone circuits — [PM Magazine: design pitfalls and piping strategies](https://www.pmmag.com/articles/106862-right-and-wrong-examining-common-design-pitfalls-and-how-to-optimize-piping-strategies)
- Conflict: Heat Geek describes "distortion" as the boiler having to run hotter (not explicitly as warming the return), whereas forum practitioners describe it as return warming; both result in lower condensing — [Heat Geek](https://www.heatgeek.com/low-loss-headers/) vs. [HeatingHelp](https://forum.heatinghelp.com/discussion/comment/1399627/)
- Alternatives to an LLH: close-coupled tees (same principle, cheaper), upsizing primary pipework/full-bore valves, a bigger pump, or a buffer (mainly for multiple heat sources); zone valves/non-return valves prevent reverse circulation through a common pipe — [Heat Geek](https://www.heatgeek.com/low-loss-headers/)
- Automatic bypass valves (ABV) maintain minimum boiler flow in TRV systems: closed while TRVs are open, opening as TRVs close; required if the boiler manufacturer specifies a minimum flow while firing — [Drayton D30-9 ABV datasheet](https://draytoncontrols.co.uk/sites/default/files/2025-04/D30-9%20Automatic%20By%20Pass%20Valve%20Datasheet.pdf); [ESi ABV installation sheet](https://checkout.mkm.com/content/products/pdf/51107/esi-automatic-bypass-valve-l039060_Installation_1.pdf)
- A weather compensator does not prevent the return temperature rising greatly when an ABV opens (anecdotal practitioner statement) — [diynot: Bypass with Alpha2 pump](https://www.diynot.com/diy/goto/post?id=2138928)
- Some boilers (e.g., Baxi HE Plus, Potterton Promax) used integral flow switches that stop the burner when all TRVs close, allowing TRVs on every radiator — [diynot / forum thread summary via search](https://www.diynot.com/diy/goto/post?id=5657974) (anecdotal)
- Danfoss-cited field tests: boilers condensed ~5% of the time without effective balancing vs ~80% with it; best condensing efficiency below ~55 °C return — [HVP magazine: Danfoss on balancing](https://www.hvpmag.co.uk/Danfoss-outlines-the-benefits-of-heating-system-balancing/13075); ~54 °C return given as condensing threshold for natural gas — [Building.co.uk](https://www.building.co.uk/how-to-operate-condensing-boilers-most-efficiently/3119440.article)

### Inferences
- In this house (system boiler with internal pump, 2 radiator circuits, 1 UFH mixing kit with its own pump), the typical European arrangement is: radiators directly on the boiler pump; UFH mixing kit teed off the primary. The UFH kit pump + boiler pump in series/parallel with no separation is exactly the case where an LLH or close-coupled tees are added. If an LLH exists, the boiler pump should be set (fixed speed / ΔT mode) so primary flow ≤ secondary flow, else return temp rises.
- A plate heat exchanger is a full hydraulic separation (used mainly for oxygen-permeable old UFH pipes); it adds a ΔT penalty (approach temperature) so the boiler must run a few K hotter. No source fetched quantified this — see Gaps.
- An ABV should be set just high enough to protect minimum flow (check boiler manual); many modern boilers with modulating pumps and internal bypass don't need an external one. A permanently open manual bypass is the worst case for condensing.

### Gaps
- No authoritative (manufacturer) quantification of efficiency loss from LLH primary/secondary mismatch for domestic boilers.
- Plate HEX application (oxygen diffusion in old PEX UFH) not researched in sources.
- Exact minimum-flow requirements of the specific boiler unknown — must come from the installer/manual.

## 2. Underfloor heating behind a thermostatic mixing valve on a radiator system; CH2 setpoint (OpenTherm ID 8)

### Takeaway
A thermostatic (fixed-point) UFH mixing kit only works when the boiler flow is above its setpoint (typically 35–50 °C); when weather compensation lowers boiler flow on mild days, the valve goes fully open on the hot port and UFH gets whatever the boiler delivers (or under-delivers). The kit normally has its own pump. OpenTherm CH2 (ID 8) only helps if the boiler firmware actually drives a second mixed circuit, which very few combi/system boilers do; for a single-circuit boiler the thermostat must send max(radiator need, UFH need + margin).

### Cited Findings
- Typical temperatures: radiator systems historically ~82 °C flow; UFH 35–60 °C depending on floor build-up; mixing valve blends UFH return with boiler flow to the set temperature — [Altecnic (Caleffi UK) UFH Compact Unit datasheet](https://altecnic.co.uk/wp/wp-content/uploads/2024/10/UFH-Compact-Unit-Data-Sheet.pdf)
- Caleffi compact UFH stations come complete with high-efficiency pump, thermostatic 3-way mixing valve with built-in sensor, and flow/return gauges — [Altecnic UFH Compact Unit](https://altecnic.co.uk/wp/wp-content/uploads/2024/10/UFH-Compact-Unit-Data-Sheet.pdf)
- Fixed-point groups hold one set temperature via a thermostatic mixing valve; an electronic (motorised) mixing valve with controller varies UFH delivery temperature with outdoor temperature, "optimizing the operation of heat pumps or condensing boilers" — [Emmeti mixing units FAQ](https://emmeti.com/en/service-area/faq-frequently-asked-questions/mixing-units)
- Typical thermostatic head ranges ~20–60 °C; Weinmann & Schanz underfloor control set setpoint range 20–50 °C — [Weinmann-Schanz underfloor control set](https://weinmann-schanz.de/de/en/Heating/Heat-output/Surface-heating-system/RTL-valves-and-boxes/Underfloor-control-set-sid2870706.html)
- ESBE publishes application drawings combining radiators and UFH on one heat source with mixing valves/controllers — [ESBE applications: radiators 6](https://esbe.eu/group/applications/radiators/radiators-6), [radiators 7](https://esbe.eu/group/applications/radiators/radiators-7)
- Danfoss FTC is an accessory for floor-heating mixing shunts (fixed-temperature control) — [Danfoss FTC](https://designcenter.danfoss.com/en-cn/products/climate-solutions-for-heating/hydronic-floor-heating/floor-heating-mixing-shunts/accessories-for-floor-heating-mixing-shunts/ftc)
- UFH needs ~3x radiator flow; boiler pumps may struggle, so UFH normally gets its own pump — [Heat Geek](https://www.heatgeek.com/low-loss-headers/)
- OpenTherm second-circuit setpoint is exposed as `t_set_ch2` in ESPHome's OpenTherm component and as controlsetpoint2 / CH2 enable in the openHAB OpenTherm Gateway binding — [ESPHome OpenTherm](https://esphome.io/components/opentherm/); [openHAB OpenTherm Gateway binding](https://www.openhab.org/addons/bindings/openthermgateway/)
- "Very few boilers allow you to control the second zone"; firmware often doesn't implement it; boilers are not required to support every protocol ID — [HA community: OpenTherm gateway thermostat thread](https://community.home-assistant.io/t/opentherm-gateway-thermostat-with-full-integration-for-home-assistant/617228/209); [ESPHome OpenTherm docs](https://esphome.io/components/opentherm/)
- ESPHome exposes `ch2_active` status flag (status bit) that can be read to see if the boiler reports CH2 activity — [ESPHome OpenTherm](https://esphome.io/components/opentherm/)
- tado user report: no heat-modulation support for underfloor zones in tado's OpenTherm implementation (anecdotal) — [tado community: OpenTherm modulation with multiple zones](https://community.tado.com/en-gb/discussion/1462/opentherm-modulation-with-multiple-zones-thermostats-ufh-radiators) (now redirects to tado help; content seen as snippet only)

### Inferences
- ID 8 = TSetCH2 (control setpoint for CH2) per the OpenTherm 2.2 spec from general knowledge; not confirmed by a fetched primary source here. On a single-circuit system boiler, the boiler will normally reply "unknown data ID" or ignore it; the thermostat firmware should probe ID 8 / read status bit CH2 and fall back gracefully.
- Rule for a single flow setpoint (ID 1) serving radiators + thermostatic UFH: when UFH zone calls, TSet ≥ UFH mixing setpoint + ~5 K (so the thermostatic valve can mix); when only radiators call, TSet follows the radiator weather curve. Running the boiler at e.g. 30 °C on mild days while the UFH kit is set to 40 °C means UFH gets 30 °C — floor under-heats but is safe (thermostatic valve cannot exceed supply temp).
- Conversely, when radiators need 60 °C in cold weather, the thermostatic valve throttles the hot port and recirculates — UFH return enters boiler return at ~UFH return temp (~30 °C), which actually helps condensing.
- "Only circulates when water temperature is suitable": some kits (or the UFH pump thermostat) block the pump until flow is hot; this means UFH demand appears only after radiators/primary warm up and the thermostat must not interpret no-UFH-flow as satisfied demand.
- A motorised mixing valve controller (e.g., ESBE CRC/90 series or Danfoss ECL style, some with their own OpenTherm/bus) would let UFH be weather-compensated independently; then the boiler setpoint = max of circuit requests, i.e., the standard "highest demand wins" cascade.

### Gaps
- Could not fetch ESBE/Watts manuals to confirm "pump enable thermostat" behaviour of specific kits.
- No confirmed list of boilers supporting OpenTherm CH2 (ID 8) — need boiler-specific testing (the project's boiler is not yet connected).
- No source found for mixing-valve controllers that are themselves OpenTherm masters/slaves in a chain (OpenTherm gateway-type mixing controllers exist in some brands but were not verified).

## 3. External DHW tank on a system boiler

### Takeaway
The cleanest option for a system boiler is the manufacturer's internal (or external) diverter kit plus a tank NTC sensor wired to the boiler: the boiler then owns DHW priority and temperature, and an OpenTherm master can see/set it via DHW setpoint (ID 56) and DHW-enable/active status bits. A 230 V tank thermostat or a separate zone-valve arrangement (UK S-/Y-plan) moves DHW control outside the boiler, so the OpenTherm thermostat sees less (and may have to manage it with relays).

### Cited Findings
- Worcester Bosch Greenstar i System: the optional integral diverter valve kit with cylinder sensor is required to enable boiler hot-water temperature control; the manual tabulates which DHW accessories (integral vs external valve) the boiler governs — [Worcester Bosch literature 6720816348](https://worcester-bosch.co.uk/support/literature/download/6720816348)
- Vaillant: cylinder with VR10 NTC sensor used for boiler-controlled DHW temperature; unplugging the cylinder sensor makes the boiler think no tank is fitted and disables DHW — [diynot (forum)](https://www.diynot.com/diy/goto/post?id=5139787) (anecdotal)
- Forum conflict on whether a 230 V tank stat can coexist with Vaillant NTC control; one workaround wires tank-stat contacts to the boiler's C1–C2 input — [diynot: fitting Vaillant VR65 and VR10](https://www.diynot.com/diy/threads/fitting-vaillant-vr65-and-vr10-to-existing-system.170233/) (anecdotal)
- tado: using OpenTherm, tado cannot also control a zone valve (i.e., OpenTherm and relay zone-valve control are exclusive in that product) — [tado community (snippet)](https://community.tado.com/en-gb/discussion/comment/45099) (anecdotal)

### Inferences
- Option A (recommended): boiler diverter kit + tank NTC. Thermostat sends DHW enable bit (ID 0 master status bit 1) and DHW setpoint (ID 56), reads DHW mode status and possibly tank temperature (ID 26). Boiler handles priority, so CH is paused during reheats — the thermostat should not treat the drop in CH flow temp as a fault.
- Option B: tank thermostat on boiler terminals — boiler still diverts, but DHW temperature is set at the tank stat; ID 56 may be ignored; ID 26 likely unavailable.
- Option C: external 2-port zone valves (S-plan) with a wiring centre — the boiler sees just "CH demand" from relays; OpenTherm thermostat can't distinguish DHW from CH and weather-compensated flow setpoint might be too low for the tank (needs ~65–70 °C flow for a 55–60 °C tank). This conflicts with low-temperature CH, so avoid in an OpenTherm design.
- Legionella cycle: should be done by the boiler (if it has the function) or scheduled via ID 56.

### Gaps
- Did not fetch a Hungarian-market boiler manual (e.g., Vaillant ecoTEC, Bosch Condens, Immergas Victrix, Ariston) confirming which DHW OpenTherm IDs they implement.
- No primary source fetched confirming ID 56/26 semantics (from general OpenTherm 2.2 knowledge only).

## 4. Multi-room demand aggregation (max-demand, averages, TRV valve feedback)

### Takeaway
Commercial systems (Honeywell evohome) use max-demand: each actuator reports a heat demand, zone demand = max of its actuators, boiler demand = max of all zones, then mapped to OpenTherm modulation (flow setpoint) or TPI for relays. Community HA solutions use counts of heating TRVs or the average valve opening. Max-demand guarantees comfort but lets one cold/mis-calibrated room drive the boiler hot; averaging is more efficient but can leave the worst room cold.

### Cited Findings
- evohome: "each actuator will tell the controller their personal heat demand (a number between 0-200)"; "the controller considers the heat demand of a zone to be the greatest of all demands for that zone"; the controller calls for heat according to the greatest zone demand; with OpenTherm it modulates (water temperature), with relay (BDR) it uses TPI; 0–200 maps non-linearly to the UI percentage — [The-Evohome-Protocol wiki: 3150 Zone Heat Demand](https://github.com/zxdavb/The-Evohome-Protocol/wiki/3150:-Zone-Heat-Demand)
- evohome reportedly ignores very low valve demands (e.g., 25% valve demand not contributing to boiler demand) — [automatedhome forum](https://www.automatedhome.co.uk/vbulletin/showthread.php?p=35420) (anecdotal, unconfirmed)
- tado: only one device is wired to the boiler (OpenTherm), all other devices wirelessly request heat from it; smart radiator thermostats can be configured to call for heat or act independently — [tado community (snippets)](https://community.tado.com/en-gb/discussion/comment/52549) (anecdotal; no official doc found on the aggregation algorithm)
- Active Heating Manager (HA add-on): counts TRVs whose `hvac_action` is heating (optionally ignoring TRVs with closed valves); "dynamic" mode sets the boiler thermostat target from the average valve position (0% → off temp; 25% → current temp + 0.5 °C; 25–100% linear to max); polling 300 s; no documented anti-short-cycle or minimum-open-valves logic — [HA community: Active Heating Manager](https://community.home-assistant.io/t/active-heating-manager-add-on-smart-trv-based-boiler-control/955093)
- Better Thermostat does not itself control the boiler; users ask how the boiler is switched off when no TRV demands — [Better Thermostat discussion #1237](https://www.github.com/KartoffelToby/better_thermostat/discussions/1237); recent releases add "pi_heating_demand valve control" — [Better Thermostat releases](https://ost.ecosyste.ms/projects/3781/releases); BT computes valve position from room and outdoor sensors — [HA community](https://community.home-assistant.io/t/perfect-heating-automation-with-sonoff-trvzb/800371)
- Moes TS0601 TRVs report valve position mostly 0% or 100% (little in between) — so position feedback from cheap TRVs is weak — [HA community: Moes TRV valve position](https://community.home-assistant.io/t/moes-trv-ts0601-valve-position-overheating/525998) (anecdotal)
- Danfoss Ally / Netatmo specific aggregation logic: not found in this research session.

### Inferences
- Recommended aggregation for this project's OpenTherm firmware:
  1. Per zone compute a demand d_i ∈ [0,1] (PI output of room error, or TRV valve position / `pi_heating_demand` where available).
  2. Flow setpoint = weather curve(T_out, room target) + room-compensation term from the **max** demand of enabled zones, but with (a) a deadband ignoring zones with tiny demand (evohome-like), (b) per-zone weights/priority so a bathroom or rarely used room can't dominate, (c) clamp to a min/max flow temperature.
  3. Boiler on/off (CH enable bit) = any zone demand above threshold, with minimum on/off times; when demand exists but is small, lower the flow setpoint rather than cycling.
- Use max for comfort-critical zones (living room reference), and treat other zones as "boost requests" capped to +N K above the weather-curve value.
- Calibration offsets: TRV-internal sensors read high near the radiator; use external room sensors (Better Thermostat approach) and per-sensor offsets.

### Gaps
- Official algorithms for tado, Netatmo, Honeywell (beyond evohome protocol wiki), and Danfoss Ally boiler-demand logic were not retrieved.

## 5. TRV pitfalls: all-closed, fighting weather compensation, hydraulic balancing, reference room

### Takeaway
If every TRV can close, the boiler either deadheads, cycles via the bypass (hot water straight to return), or needs a flow switch; the standard fix is a reference room (with the room sensor) whose radiator has no TRV (or lockshield only), plus correctly set ABV. TRVs fight weather compensation: they throttle early, reduce emitter area, and force higher flow temps; WC works best as an "open" system with TRVs set above target (as limiters). Balancing is what gives low return temperatures (condensing ~80% vs ~5% of time in Danfoss-cited field data).

### Cited Findings
- Reference room: holds the main boiler thermostat; its radiator is never turned off because the boiler control depends on it — [BuildHub forum: are TRVs really a good thing](https://forum.buildhub.org.uk/topic/30469-are-trvs-really-a-good-thing) (anecdotal)
- With all TRVs closed, the flow route is closed — "trying to dump xx kW through a single radiator and a buffer"; recommended "load compensation, to reduce the flow temperature before you hit the room setpoint", reducing overshoot and cycling — [BuildHub forum](https://forum.buildhub.org.uk/topic/30469-are-trvs-really-a-good-thing) (anecdotal)
- Without a linked control, a boiler cannot know all TRVs are closed and keeps cycling until the bypass lifts, sending hot water direct back to the boiler — [diynot: weather compensation what does it do?](https://www.diynot.com/diy/goto/post?id=5657974) (anecdotal)
- With weather compensation, "you NEED an open system… rad stats should be moved to say 23 °C if your room target is 20" — [diynot: weather compensation](https://www.diynot.com/diy/goto/post?id=5657920) (anecdotal)
- Heat Geek: TRVs/room stats closing early is like "breaking and accelerating at the same time"; TRVs are slightly better than room stats because they throttle proportionally; with room-sensor compensation a TRV fighting the sensor forces the controller to raise flow temperature; "The lower the flow temperature… the more efficient it is"; zoning raises flow temperature because fewer emitters must deliver the heat; gas boilers lose much less per start than heat pumps but zoning still increases cycling — [Heat Geek: why not to zone heat pumps or boilers](https://heatgeek.com/articles/why-not-to-zone-heat-pumps-or-boilers)
- Heat Geek worked example: setting 2 of 4 rooms to 18 °C cuts heat loss only ~6.3% (≈4.2% once inter-room heat transfer is counted) but forces active radiators' mean water temperature from ~46 °C to ~51.5 °C; fully off rooms push flow to ~64 °C — [Heat Geek](https://heatgeek.com/articles/why-not-to-zone-heat-pumps-or-boilers)
- Setback rather than off: with WC, drop to setback (up to ~3 K for gas boilers) to keep mass warm and flow low — [Heat Geek](https://heatgeek.com/articles/why-not-to-zone-heat-pumps-or-boilers)
- Balancing: Heat Geek says balancing doesn't itself make the boiler condense, but an overspeed pump raises return temperature; balance via measured ΔT — [Heat Geek: balancing heating systems](https://www.heatgeek.com/balancing-heating-systems-the-ultimate-guide/); Danfoss-cited field tests: boilers condense ~5% of the time unbalanced vs ~80% balanced — [HVP magazine](https://www.hvpmag.co.uk/Danfoss-outlines-the-benefits-of-heating-system-balancing/13075)
- Danfoss balancing application guide (dynamic/pressure-independent valves) — [Danfoss BE174986460861](https://assets.danfoss.com/documents/57229/BE174986460861en-010101.pdf) (not fetched in detail)
- Sonoff published a multi-device TRV automation guide for whole-home heating — [Sonoff advanced TRV guide](https://sonoff.tech/en-br/blogs/news/advanced-trv-guide-multi-device-automation-for-whole-home-heating) (not fetched)

### Inferences
- For the firmware: when smart TRVs are added, track "open emitter fraction"; if all non-reference TRVs are closed and the reference room is satisfied, drop the CH enable bit (don't just lower setpoint) — this avoids bypass-only circulation. If any TRV is open but total demand is low, lower TSet to the boiler's minimum modulating point instead of on/off.
- Smart TRVs should be driven from external room sensors with setpoints such that the TRV acts as a limiter (target + 1–2 K) when WC is active, so they don't fight the curve.
- Dynamic/pressure-independent TRV inserts (e.g., Danfoss Dynamic Valve, Heimeier Eclipse) automate balancing; useful when retrofitting TRVs in Phase 3.

### Gaps
- No manufacturer document fetched quantifying condensing loss from ABV opening.
- Danfoss Ally, Netatmo "boiler demand from TRV" details not verified.

## 6. Phased rollout recommendations and installer questions

### Takeaway
Phase 1: single reference-room sensor + weather compensation via OpenTherm flow setpoint (ID 1), UFH kit fixed setpoint, DHW via boiler kit. Phase 2: additional room sensors used as limiters/boost requests with deadband and capped influence. Phase 3: TRVs with valve-position feedback, max-demand with thresholds, plus balancing. Most of this is supported by the sources above; the phase structure itself is the researcher's synthesis.

### Cited Findings
- Max-demand aggregation with OpenTherm modulation is the established commercial approach — [Evohome protocol wiki 3150](https://github.com/zxdavb/The-Evohome-Protocol/wiki/3150:-Zone-Heat-Demand)
- Only one device should be wired to the boiler; others send requests to it — [tado community](https://community.tado.com/en-gb/discussion/comment/52549) (anecdotal)
- Low flow/return temperatures and avoiding zoning-induced high flow temperatures are the main efficiency levers — [Heat Geek](https://heatgeek.com/articles/why-not-to-zone-heat-pumps-or-boilers); [HVP/Danfoss](https://www.hvpmag.co.uk/Danfoss-outlines-the-benefits-of-heating-system-balancing/13075)
- Boilers may not implement all OpenTherm IDs (e.g., CH2) — [ESPHome OpenTherm](https://esphome.io/components/opentherm/)
- DHW temperature control via boiler requires the diverter kit + cylinder sensor on Bosch — [Worcester Bosch manual](https://worcester-bosch.co.uk/support/literature/download/6720816348)

### Inferences
- Phase 1 (one sensor): pick the reference room with no TRV (or TRV fully open); control = weather curve + room-compensation (PI on room error trimming TSet), min on/off timers, max TSet limit. Ensure TSet ≥ UFH kit setpoint + ~5 K whenever UFH is in heating season (or accept UFH under-delivery on mild days). Log boiler return temp (ID 28) to verify condensing (<~55 °C).
- Phase 2 (more sensors): each extra room contributes a capped boost (e.g., ≤ +5 K on TSet) only if its error exceeds a deadband for N minutes; per-room weights/priority; optional exclusion of rooms with other heat sources. Avoid true max-demand from unbalanced rooms — fix the balance instead.
- Phase 3 (TRVs): Zigbee TRVs (Sonoff TRVZB, Aqara, Danfoss Ally) via HA; use external sensor + Better Thermostat-style calibration; feed valve positions; boiler CH on if any non-reference valve > threshold (e.g., 20–30%) or reference room demands; TSet from weighted max of demands; CH off if all closed; keep reference radiator TRV-free; rebalance (lockshields or dynamic valves) after fitting.
- Installer questions:
  1. Pump: internal pump head/type and mode (proportional/constant pressure/ΔT)? Is there any external pump?
  2. Is there a low-loss header/close-coupled tees/plate HEX? Why? How are primary vs secondary flows set?
  3. Is there an automatic bypass valve (or permanent bypass)? Set pressure? Does the boiler have internal bypass/min flow spec?
  4. UFH kit: make/model, thermostatic or motorised, set temperature, own pump, does it have a pump-enable thermostat or relay from the room control? Is UFH pipe oxygen-barrier?
  5. DHW: boiler diverter kit + NTC sensor, or tank stat, or zone valves? Who sets tank temperature? Legionella function?
  6. Outdoor sensor: fitted/wired to boiler? Can the OpenTherm master read it (ID 27) or must it supply its own?
  7. OpenTherm: are OT terminals accessible and free (no bridge/relay thermostat installed)? Which OT version? Does the boiler support CH2 (ID 8)? Remove link on 230 V/24 V on-off terminals when OT is used?
  8. Hydraulic balancing: was it done, with what method; are lockshields/presettable TRV bodies installed?
  9. Minimum boiler output (kW) vs smallest zone load — risk of cycling in mild weather.

### Gaps
- Hungarian/EU-specific regulations (e.g., EU ErP class VI controls, Hungarian building rules on TRVs) not researched.
- CIBSE / Energy Saving Trust guidance not retrieved within the tool budget.
