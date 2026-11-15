# Phase 1 control research: study guide

This folder holds the research and study material for **Phase 1**: the thermostat drives the new **Immergas Victrix Tera 24 Plus** over OpenTherm, controlled from the SHT40 room sensor. Reliability comes first, then efficiency as far as the sensors allow. It was researched in November 2026; **no firmware was changed**.

> **The short version**
> 1. **Buy the V2.** Only the *Victrix Tera **V2** 24 Plus* has installer parameter **P7 = 2** (a "commercial", i.e. third-party, remote on terminals 44/41). On the original model P7 is "not available" ([ch. 03](03-victrix-tera-24-plus.md)).
> 2. **Immergas speaks "OpenTherm, modified".** The firmware needs three quirks:
>    - status request low byte **0xCA**;
>    - mirror the CH setpoint into ID 57;
>    - send setpoint 10 °C whenever CH is off.
> 3. **This boiler fails cold.** If the thermostat dies, the boiler raises E31 and, after a power cycle, refuses central heating. Plan a **watchdog-driven fallback relay** ([ch. 11 in the full report](full-report.md#11-safety-the-boiler-owns-the-hazards-the-firmware-owns-the-failure-direction), [ch. 10](10-recommendation-phase1.md)).
> 4. **Phase 1 control:** a load-compensating PI turns the room error into the flow setpoint (ID 1), plus **strict anti-cycling**. The boiler's 4.3 kW minimum is bigger than the house's heat loss on mild days, and short burns cost as much efficiency as modulation gains. Add a weather curve when an outdoor temperature is available.
> 5. **Machine learning comes later**, as a simple thermal model (optimum start, self-tuning curve). That only works if everything is **logged from day one**, gas meter included.

## Reading path

| # | Chapter | You'll learn | Interactive |
|---|---|---|---|
| 01 | [Heating basics](01-heating-basics.md) | Heat loss, radiators, condensing, why return temperature is the efficiency dial | [condensing.html](interactive/condensing.html) |
| 02 | [OpenTherm in depth](02-opentherm.md) | The wire, the frame, the timing, every Data-ID that matters, the failure modes | [opentherm-frame.html](interactive/opentherm-frame.html) |
| 03 | [Victrix Tera 24 Plus and Immergas](03-victrix-tera-24-plus.md) | V1 vs V2, P7, the 0xCA quirk, parameters, E31 | – |
| 04 | [Control strategies](04-control-strategies.md) | On/off → PI → weather curve → combined; tuning; setback | [heating-curve.html](interactive/heating-curve.html), [room-sim.html](interactive/room-sim.html) |
| 05 | [Low load and short cycling](05-low-load-cycling.md) | Why the 4.3 kW floor matters most, and how to manage it | [room-sim.html](interactive/room-sim.html) |
| 06 | [DHW with an external tank](06-dhw-tank.md) | Tank NTC, priority, legionella, what the thermostat controls | – |
| 07 | [Hydraulics, zones and TRVs](07-hydraulics-zones-trvs.md) | Headers, bypasses, the underfloor mixer, room aggregation, TRV pitfalls | [heating-curve.html](interactive/heating-curve.html) |
| 08 | [Reference implementations](08-reference-projects.md) | What OTGW, ESPHome, SAT, OTGateway, Nest, tado… do | – |
| 09 | [Learning, data and efficiency KPIs](09-learning-data-kpis.md) | RC models, MPC reality check, what to log, how to measure savings | – |
| 10 | [Recommendation for Phase 1](10-recommendation-phase1.md) | The proposed design, roadmap, boiler test plan, installer questions | – |

Supporting material:
- [full-report.md](full-report.md): the complete research report (about 12k words), with every claim cited inline.
- [notes/](notes/): the raw research notes per topic, with findings, inferences and gaps.
- [sources.md](sources.md): the key sources, grouped.

## How to use the interactive pages

GitHub shows `.html` files as source and doesn't run them. Open them locally from a clone instead:

```sh
git clone git@github-opentherm-thermostat:nthnghu/opentherm-thermostat.git   # or pull
open docs/research/interactive/room-sim.html                             # macOS; any browser works
```

The pages are self-contained: no build step, no network access, light and dark themes. If the repository is ever made public, GitHub Pages can serve them directly.

Inside the Markdown chapters, look for:
- **Mermaid diagrams**, rendered by GitHub.
- **▶ Deeper** sections: click to expand the details and worked examples.
- **✅ Check yourself** questions, with the answers hidden in a collapsible block.
- **🔧 What this means for our thermostat**: the design consequence of each chapter.
- **⚠️ Verify on the real boiler**: things the research could not confirm. They are tested once the boiler is installed (test plan in [ch. 10](10-recommendation-phase1.md#verify-on-the-real-boiler)).

## Glossary

| Term | Meaning |
|---|---|
| **CH** | Central heating (space heating) |
| **DHW** | Domestic hot water |
| **Flow / return** | Water leaving the boiler towards the radiators / coming back |
| **ΔT** | Temperature difference, usually flow − return, or mean radiator temperature − room |
| **TSet (ID 1)** | The CH flow setpoint the thermostat sends over OpenTherm |
| **Modulation** | Boiler burning at partial power, here between 4.3 and 24 kW |
| **Turndown** | Max ÷ min output (≈ 5.6 : 1 for the Tera) |
| **Short cycling** | Burner switching on and off every few minutes; wasteful and wearing |
| **Load compensation** | Flow temperature set from the *room* error (indoor sensor) |
| **Weather compensation** | Flow temperature set from the *outdoor* temperature via a heating curve |
| **Heating curve** | Function outdoor temp → flow temp (slope plus shift) |
| **PI / PID** | Proportional-integral(-derivative) controller |
| **Dew point** | ≈ 55.6 °C for natural gas flue gas; the return must be below it to condense |
| **NCV / GCV** | Net / gross calorific value; NCV efficiencies can exceed 100 % |
| **OT/+ / OT/−** | Full digital OpenTherm / simple PWM "OpenTherm Lite" |
| **IMG BUS** | Immergas's own OpenTherm-like remote-control bus |
| **E31** | Tera error: lost communication with the remote control |
| **OPV** | Overshoot protection value: the flow temperature the boiler settles at on minimum fire (SAT term) |
| **t4** | Tera installer parameter: anti-cycling (re-ignition delay) timer |
| **TRV** | Thermostatic radiator valve, mechanical or smart |
| **Low-loss header** | Hydraulic separator between boiler and system pumps |
| **RC model** | Resistance-capacitance thermal model of a building (1R1C, 2R2C…) |
| **MPC** | Model predictive control |
| **HDD** | Heating degree days, used to weather-normalise energy use |
| **ErP class** | EU control-class rating (I–VIII) for temperature controls |
