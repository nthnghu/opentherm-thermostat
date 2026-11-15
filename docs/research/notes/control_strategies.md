# Control strategies for modulating condensing gas boilers driven by an OpenTherm room controller

Scope: one house, two radiator circuits plus one underfloor circuit, an indoor sensor now and possibly an outdoor sensor later, Hungary (EU). Sources were fetched in Nov 2026. SAT (Home Assistant "Smart Autotune Thermostat") details come from reading its source at commit 7d43560 (1 Mar 2026), cloned from https://github.com/Alexwijn/SAT.

## Q1. Condensing physics: dew point, efficiency vs return temperature, radiator output vs water temperature

### Takeaway
A gas boiler condenses only when the water returning to the heat exchanger is below about 55 °C (flue dew point about 55.6 °C), and its efficiency keeps rising as the return temperature falls. Radiator output follows (ΔT/50)^1.3, so in mild weather a radiator system needs only modest water temperatures. Lowering the flow temperature (with a large ΔT) whenever the load allows is the main physical lever.

### Cited Findings
- The flue gas dew point of natural gas (H) is about 55.6 °C. Flue gases must be cooled below it to recover latent heat. Bosch's efficiency curves plot boiler efficiency and condensate volume against return-water temperature. — [Bosch Industrial whitepaper "Condensing"](https://www.bosch-industrial.com/africa/media/country_pool/knowledge/technical-reports/whitepaper_condensing.pdf)
- BRE (for BEIS, 2017), Figure 2, "Theoretical relationship between boiler efficiency and return water temperature by fuel type": gas boilers start to condense below about 55 °C, LPG below about 48 °C and oil below about 45 °C. The gradient changes at that point, and below it the efficiency gain becomes more substantial. The y-axis spans roughly 84–100 % gross over return temperatures of 20–80 °C. The figure comes from BRE's SAP consultation paper CONSP:02. — [BRE Client Report 1178/06/2016, "Evidence Gathering – Compensation and TPI Heating Controls", Sept 2017, pp. 10–11](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)
- BRE also notes that lower water temperatures reduce pipework heat losses and the large temperature swings of standard on/off control. — [BRE 2017, p. 11](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)
- Efficiency falls as return temperature rises. Above about 138 °F (about 59 °C), roughly the flue dew point, a condensing boiler runs at about 87 % efficiency. — [CSE Magazine, "Retrofitting with condensing boilers"](https://www.csemag.com/retrofitting-with-condensing-boilers/)
- Rule-of-thumb figures (not measured): about 96–98 % at 80 °F (27 °C) return, 88–90 % at 130 °F (54 °C), 85–87 % at 140 °F (60 °C). — [boilerconnection.com guide](https://boilerconnection.com/guides/condensing-boiler-guide) (low-quality source, illustrative only)
- Heat Geeks: condensing starts below a 55 °C return, so keep the flow below about 55 °C. Most boilers turn down to between a quarter and a tenth of maximum output. Examples given: a Viessmann 200-W 32 kW combi and an 11 kW system boiler both have a 1.9 kW minimum. — [Heat Geeks, "This will make you think again about how efficient your brand new boiler really is"](https://www.heatgeek.com/articles/this-will-make-you-think-again-about-how-efficient-your-brand-new-boiler-really-is)
- Condensing boilers can give 10–15 % efficiency gains, "particularly when lower system water temperature" is used. Cited radiator-tuning work raised the gain from an initial 15 % up to 30 %. — [Bennett, Elwell, Oreszczyn (UCL), "Space heating operation of combination boilers in the UK" (BSERT)](https://discovery.ucl.ac.uk/10054754/1/StartStop_BG_Paper_BSERT_v4_final_main_updated_figures_references.pdf)
- Radiator output (EN 442 rating method): Q = Q_rated × (ΔT/ΔT_rated)^n, where ΔT = (T_flow + T_return)/2 − T_room, ΔT_rated = 50 K and n ≈ 1.3. — [Trystan Lea, radiators for heat pumps](https://trystanlea.org.uk/heatpumpradiators); [Zehnder delta-T correction factors](https://www.zehnder.co.uk/en/decorative-radiators/resources/correction-factors-delta-t-conversion)
- The EN 442 rating point is a mean water temperature of 70 °C at 20 °C room (75/65/20 or 80/60/20). Zehnder assumes n = 1.3. — [Zehnder](https://www.zehnder.co.uk/en/decorative-radiators/resources/correction-factors-delta-t-conversion)
- A measured double-panel radiator fitted n ≈ 1.5 better than 1.3, so one exponent may not suit every emitter. — [OpenEnergyMonitor forum, "Rating radiators at off-design conditions"](https://community.openenergymonitor.org/t/rating-radiators-at-off-design-conditions/27313); [REHVA Journal, "Can we still trust in EN 442?"](https://rehva.eu/rehva-journal/chapter/can-we-still-trust-in-en-442-new-operating-definitions-for-radiators-part-2-model-based-analysis-and-results)
- Some web calculators contain arithmetic errors. Check: (22.5/50)^1.3 ≈ 0.35, not 0.39. — [OpenEnergyMonitor thread](https://community.openenergymonitor.org/t/rating-radiators-at-off-design-conditions/27313); [squote](https://squote.app/knowledge/calculators/heat-pump-radiator-correction) (contains the error)

### Inferences
- Worked numbers from the EN 442 formula (n = 1.3, room 20 °C), as a fraction of the ΔT50 rating:
  - mean water 70 °C (ΔT 50) → 1.00
  - 60 °C (ΔT 40) → 0.75
  - 55 °C (ΔT 35) → 0.63
  - 50 °C (ΔT 30) → 0.51
  - 45 °C (ΔT 25) → 0.41
  - 40 °C (ΔT 20) → 0.30
  - 35 °C (ΔT 15) → 0.21

  A radiator system sized for 75/65 at the design outdoor temperature therefore needs only a mean water temperature of about 45 °C when the heat load is about 40 % of design, which is typical of shoulder-season days.
- Sizing check for this house: if the radiators' ΔT50 ratings sum to P_rad and the design heat loss is P_design, the mean water temperature needed at design is T_mean = 20 + 50 × (P_design/P_rad)^(1/1.3). For example, radiators rated at 1.5× the heat loss need T_mean ≈ 20 + 50 × 0.667^0.77 ≈ 20 + 36.6 ≈ 57 °C at design. With a 10 K ΔT that is roughly 62/52, and with a 20 K ΔT roughly 67/47 (return below 55 °C, so it condenses even at design).
- Large ΔT helps condensing because, at a fixed mean temperature, the return is lower. Example: mean 50 °C at ΔT 20 K gives a 40 °C return, against 45 °C at ΔT 10 K. The required pump flow is also lower, since ṁ = Q/(c_p·ΔT).
- The underfloor circuit needs much lower water temperatures (SAT recommends a max of 50 °C; see Q2). Mixing it from the boiler flow returns cool water. However, if the boiler flow setpoint is driven by the radiators, the mixing valve gives no efficiency benefit beyond what the radiators already allow.

### Gaps
- No primary EN 15502 / Viessmann tabulated "efficiency vs return temperature" numbers were retrieved. The BRE figure was available only as a plot (axis ranges, no data table). The commonly cited ~98 % NCV-equivalent / ~97 % gross at a 30 °C return could not be verified from a fetched source here.
- Exact BRE curve points for natural gas (e.g. % gross at 30, 40, 50, 60 °C return) were not extractable from the PDF text.

## Q2. Control strategies: on/off, TPI, modulating PI/PID, weather compensation, combined weather+room, optimum start, setback

### Takeaway
Ranked by efficiency potential, from evidence and regulation:
1. Load compensation (room-error → flow setpoint, i.e. a PI controller over OpenTherm ID 1).
2. Weather compensation plus room influence. This is the best combination and the target for this project once an outdoor sensor exists.
3. TPI, then plain on/off.

The modelled efficiency gains grow strongly as design flow temperatures fall: about 1–2 % at 80/60, about 3–5 % at 55/47 and about 6–8 % at 45/38. Measured savings evidence is thin and mostly test-house based.

### Cited Findings
**On/off thermostat**
- An on/off controller needs a differential between the upper and lower limits, which causes temperature swing. The typical swing is about 3 °C for mechanical thermostats and about 1–1.5 °C for electronic ones. — [BRE 2017, p. 9](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)
- A 1 °C reduction in the temperature setting can save "around 10%" energy. Tighter control that removes under-heating may let occupants lower the setpoint. — [BRE 2017, §5.1, p. 23](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)
- Simple on/off room controllers give no modulating signal, so overshoot is to be expected. Proportional controls that estimate power demand from the room error are the recommended improvement. — [Bennett et al., UCL](https://discovery.ucl.ac.uk/10054754/1/StartStop_BG_Paper_BSERT_v4_final_main_updated_figures_references.pdf)

**TPI (time-proportional and integral)**
- Definition: on/off cycling within a constant cycle period, with the on:off ratio proportional to the room error plus an integral term. Example: in a 10-minute cycle, a call for 40 % output gives 4 min on and 6 min off. TPI operates only within about 1 °C of setpoint. — [BRE 2017, §2.5, p. 14](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)
- The key risk is extra boiler wear and noise from cycling, so the cycle time should be set to avoid wear. — [BRE 2017, §2.6](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)
- Evidence for TPI:
  - O'Hara/Danfoss environmental chamber: TPI at 6 cycles/h saved about 10.35 % against a mechanical on/off thermostat and about 8.25 % against an electronic on/off thermostat.
  - EST/Gastec field trial of about 50 homes: seasonal efficiency rose from 82.86 % to 83.21 %, which was not statistically significant (savings 0–0.5 %).
  - BEAMA 2014 test house: 33 % saving, but against a "no thermostat" base case with significant overheating.

  — [BRE 2017, §5.7, pp. 32–33 and Table 3, p. 36](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)

**Load compensation (modulating room control)**
- Compensation control estimates heat demand and limits the flow temperature. Weather compensation uses the outdoor temperature and load compensation uses the indoor temperature. The saving comes from lower return temperatures at low demand. — [BRE 2017, §2.3](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)
- Evidence: O'Hara (2009) environmental chamber, 10 %/14 % savings against a room thermostat at 20 °C steady state. — [BRE 2017, Table 3](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)
- BEAMA-commissioned Salford tests (2021): the largest savings came from load compensation with direct boiler modulation. Weather compensation with direct modulation (Class VI) gave similar savings to Class V. — [HVP magazine on BEAMA tests](https://www.hvpmag.co.uk/BEAMA-examines-energy-saving-potential-of-load-and-weather-comp-controls/12769); [BEAMA test report page](https://beama.org.uk/resourceLibrary/salford-tests-on-load-and-weather-compensation-.html)
- BEAMA's fuel-bill table uses 10 % savings for a Class IV upgrade and 12 % for Class V/VI upgrades, against a Class I baseline. — [BEAMA, Estimated fuel bill savings](https://www.beama.org.uk/static/25a01cf4-39d5-45ed-b7f67c57d0a08146/Estimated-fuel-bill-savings-from-heating-control-upgrades.pdf) (industry body; figures derive from its own commissioned tests)

**ErP / SAP credits (regulatory, not measured)**
- Ecodesign Reg. 811/2013 temperature control classes give these contributions to seasonal efficiency:

  | Class | Control | Contribution |
  |---|---|---|
  | I | on/off room thermostat | 1 % |
  | II | weather compensation, modulating | 2 % |
  | III | weather compensation, on/off | 1.5 % |
  | IV | TPI | 2 % |
  | V | modulating room thermostat (load compensation) | 3 % |
  | VI | weather compensation + room sensor, modulating | 4 % |
  | VII | weather compensation + room sensor, on/off | 3.5 % |
  | VIII | multi-sensor modulating room control | 5 % |

  BRE says the technical origin of these values is not stated, "so their usefulness is questionable". With no control at all, the ErP method subtracts 3 %. — [BRE 2017, Table 1, p. 25](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)
- SAP gives a 3 % credit to condensing gas boilers with enhanced load compensators and no credit for TPI. — [BRE 2017, p. 25](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)
- BRE/SAP 2016 modelled efficiency gains over a standard room thermostat for gas boilers, by design flow/return:

  | Control | 80/60 | 55/47.1 | 45/38.6 | 35/30 |
  |---|---|---|---|---|
  | WC modulating (II) | 0.7 % | 3.4 % | 6.1 % | 8.0 % |
  | Load comp. modulating (V) | 0.7 % | 3.4 % | 6.1 % | 8.0 % |
  | WC + room, modulating (VI) | 1.8 % | 5 % | 6.9 % | 8.4 % |
  | WC + room, on/off (VII) | 2.3 % | 5.4 % | 7.2 % | 8.7 % |

  — [BRE 2017, Table 2, p. 28](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)
- Most modern boilers can modulate down to 30 % of maximum output or lower. The modulation range limits how low the return temperature can go, and so limits the compensation benefit. — [BRE 2017, §5.2.1](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)

**Weather compensation (heating curve)**
- The simplest form is a single fixed-shape curve that can be shifted (parallel displacement). Advanced controls offer a family of curves (slope selection). Plain weather compensation is open-loop and must be tuned at commissioning. — [BRE 2017, §2.3.1](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)
- Siting the outdoor sensor: out of direct sun, away from windows, doors, vents, gutters and balconies, about 2–2.5 m above ground. Online weather data is less accurate, which "possibly accounts for" most online-weather controls also using load compensation. — [BRE 2017, p. 12](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)
- Drawbacks of weather compensation alone: it must be tuned, and with intermittent schedules in mild weather it takes longer to reach setpoint. The fix is to combine it with an indoor sensor (load compensation), which raises the water temperature on mild days. Weather compensation with a separate timer is less efficient, and it is not compatible with a separate optimum start/stop control. — [BRE 2017, pp. 14 and 24](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)
- Weather compensation evidence: a confidential Viessmann/Salford test house reported 15–45 % savings against a room thermostat with TRVs, but the internal temperature was not reached with weather compensation, which inflates the result. Manufacturer literature quotes 10–40 %. — [BRE 2017, §5.5 and Table 3](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)
- Siemens RVL470: slope s = (T_flow at −5 °C − T_flow at +15 °C) / 20 K, valid for a 20 °C room setpoint. Parallel displacement shifts the flow temperature equally at all outdoor temperatures and is the correction for a room that is consistently too warm or too cold. — [Siemens RVL470 manual §8.6](https://www.manualslib.com/manual/958756/Siemens-Rvl470.html?page=35); [Siemens LMS14 manual](https://www.manualslib.com/manual/1308475/Siemens-Lms14.html?page=137)
- Siemens LMS14 (as reported in a search summary; verify against the manual): T_flow = T_R + [2 + (T_R − T_A,comp) − 0.005·(T_R − T_A,comp)²]·s. The curve is non-linear, practically linear at small slopes (underfloor), and uses a composite (damped) outdoor temperature. — [Siemens LMS14 p. 137](https://www.manualslib.com/manual/1308475/Siemens-Lms14.html?page=137), [p. 143](https://www.manualslib.com/manual/1308475/Siemens-Lms14.html?page=143)
- Vaillant describes the heating curve by two parameters. Slope sets how fast the flow temperature rises as the outdoor temperature falls. Level shifts the whole curve. — [Vaillant help centre: heating curve](https://www.vaillant.com/en/advice/help-center/heating-curve/)
- Danfoss ECL derives the slope from design outdoor temperature, design flow temperature and design room temperature. — [Danfoss ECL Comfort 110 manual, p. 11](https://manualslib.com/manual/1369045/Danfoss-Ecl-Comfort-110.html?page=11)

**Combined weather + room influence**
- Siemens RVL470 room influence uses a gain from 0 (room deviation has no effect) to 20 (maximum effect). — [Siemens RVL470 §8.5.2](https://www.manualslib.com/manual/958756/Siemens-Rvl470.html?page=33)
- Siemens Synco 700 gives an approximation: Δflow setpoint ≈ Δroom setpoint × influence factor × (slope + 1). — [Siemens Synco700 RMK770 manual](https://manualslib.com/manual/1226667/Siemens-Synco700-Rmk770.html?page=163)

**SAT (Home Assistant) as a concrete open-source reference implementation of "curve + PID → OpenTherm ID 1"** (read from source)
- Heating curve: `curve = base + (coef/4) × [4·(T_target − 20) + 0.03·(T_out − 20)² − 0.4·(T_out − 20)]`. The base is 27.2 °C for radiators and 20 °C for underfloor. The default coefficient is 2.0. — [SAT heating_curve.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/heating_curve.py); [const.py defaults](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/const.py)
- Control setpoint sent to the boiler = max(curve + PID_output, 10 °C), smoothed with an exponential filter. — [SAT climate.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/climate.py)
- Recommended maximum water setpoint: 55–75 °C for radiators and 50 °C for underfloor. SAT's three curve variants ("Classic", "Quantum", "Precision") each come in radiator and underfloor versions, and "Precision" is recommended. — [SAT README](https://github.com/Alexwijn/SAT)

**Optimum start / night setback**
- DOE Building America (hydronic): about 1 % energy saving per 1 °F of setback over an 8-hour night, with more in colder weather. It advises against setback in very well-insulated homes, where savings are small. — [DOE Building America case study: hydronic setback](https://www.energy.gov/sites/prod/files/2014/07/f17/ba_case_study_hydronic_setback.pdf)
- DOE/NREL Measure Guideline "Condensing Boilers – Optimizing Efficiency and Response Time During Setback Operation" addresses the conflict between setback recovery (high flow temperature) and condensing efficiency. — [OSTI 1126299](https://www.osti.gov/scitech/servlets/purl/1126299)
- Modelling of an office building found night setback increases the hours a boiler operates in condensing mode. — [CSE Magazine](https://www.csemag.com/retrofitting-with-condensing-boilers/)
- Warming a building up as fast as possible is not necessarily optimal for energy, cost or carbon. For high-mass buildings the morning recovery load can offset setback savings. — [eScholarship paper](https://escholarship.org/content/qt6zw3x4rt/qt6zw3x4rt.pdf?t=sivjur) (search summary only; paper not read in full)
- A Salford test (via BRE) showed that lowering the TPI setpoint, rather than switching the heating off, can reduce energy consumption. — [BRE 2017, §5.7.1](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)

### Inferences
- For this house with only an indoor sensor now, load compensation is the evidence-backed choice. That means PI(D) on room error producing an OpenTherm ID 1 flow setpoint, bounded to roughly 25–55/60 °C for radiators. It corresponds to ErP Class V and the modulating "load compensation" row of the SAP table.
- With an outdoor sensor later, move to curve + PI trim (Class VI). This is the SAT architecture and Siemens' "room influence". The curve supplies the feed-forward and the PI removes curve error and internal or solar gains. The integral term should be bounded so it cannot drift far from the curve (SAT clamps it to ± the curve value).
- Both radiator circuits and the underfloor circuit are fed from one boiler flow. The boiler setpoint should follow the highest-demand circuit (normally the radiators), and the underfloor loop should be limited by its own mixing valve or thermostat. If the underfloor circuit has its own pump/mixer, the second OpenTherm channel (ID 8, TsetCH2) is relevant only if the boiler supports CH2. Most domestic boilers do not, so treat this as unverified.
- Night setback with a condensing boiler: deep setback forces a high-temperature recovery, which defeats condensing. A shallow setback (1–3 K), or an optimum-start ramp that limits the flow setpoint during recovery, keeps most of the setback saving without leaving condensing mode. This is reasoned from the cited facts, not from a controlled study.
- Weighting the evidence: test-house and chamber savings (10–15 %) are much larger than the only field trial (TPI, about 0.4 %, not significant). Expect real gains from modulation and compensation of a few percent of gas use. The bigger wins come from lower average room temperature and avoiding cycling.

### Gaps
- No primary Viessmann or Vaillant curve equations were retrieved. Only the Siemens and SAT formulas are documented here, and the LMS14 formula comes from a search summary.
- No controlled study found on optimum start for domestic condensing boilers, or on night setback against constant temperature with separate heavy- and light-building results and percentages.
- The BEIS Boiler Plus policy review (2021) was not read in detail: [link](https://assets.publishing.service.gov.uk/media/611b9287d3bf7f63b086cb04/boiler-plus-policy-review.pdf).
- Hungarian design outdoor temperature (believed to be about −13 °C under MSZ EN 12831 for most of the country) is not verified from a fetched source.

## Q3. The low-load problem: minimum output above heat loss, short cycling and mitigations

### Takeaway
Short cycling is the dominant real-world inefficiency. Field data shows a median of 53 starts/day, with 70 % of boilers averaging under 10-minute runs. Lab data shows efficiency losses of 1.5 % at 3-minute burns rising to 4 % at 1-minute and 12 % at 10-second burns. The mitigations are:
- lower the flow setpoint but keep it above the level at which the boiler at minimum modulation overshoots;
- cap maximum modulation (ID 14);
- impose minimum on-times, minimum off-times and a starts-per-hour cap;
- use duty-cycled flow setpoints (the SAT PWM approach) when demand is below minimum output.

### Cited Findings
- Analysis of 221 UK combis: half averaged more than 50 starts/day, and 70 % of starts in space heating averaged under 10 minutes. The median was 53 starts/day and the mean 93 (about 4 starts/hour). Most combis are oversized for space heating and "despite available modulation are unable to prevent rapid on-off cycling". — [Bennett, Elwell, Oreszczyn (UCL)](https://discovery.ucl.ac.uk/10054754/1/StartStop_BG_Paper_BSERT_v4_final_main_updated_figures_references.pdf)
- Effect of burner on-time per cycle on gross efficiency (lab data cited by UCL):

  | Burner on-time | Change in gross efficiency |
  |---|---|
  | 3600 s | 0 % |
  | 180 s | −1.5 % |
  | 120 s | −2.3 % |
  | 60 s | −4.1 % |
  | 30 s | −6.8 % |
  | 10 s | −11.8 % |

  Losses come from fixed pre- and post-purge times. Unburnt hydrocarbon emissions with about 150 s cycles were 95.6 mgC/kWh, against 0.8 mgC/kWh near steady state, about 120×. — [UCL paper, Table 1](https://discovery.ucl.ac.uk/10054754/1/StartStop_BG_Paper_BSERT_v4_final_main_updated_figures_references.pdf)
- When demand is below about 20 % of boiler capacity, minimum modulation (typically 20–30 % of maximum) delivers more energy than needed. The return temperature rises until the maximum supply temperature is reached and the boiler cycles. A typical modulation range is 1:6 (e.g. 4–24 kW), and newer designs reach 1:10 (2.4–24 kW). — [UCL paper](https://discovery.ucl.ac.uk/10054754/1/StartStop_BG_Paper_BSERT_v4_final_main_updated_figures_references.pdf)
- Boilers have built-in anti-cycling functions, such as a minimum time between burner starts. — [UCL paper](https://discovery.ucl.ac.uk/10054754/1/StartStop_BG_Paper_BSERT_v4_final_main_updated_figures_references.pdf)
- Heat Geeks oversizing example: a 40 kW boiler with a 10 kW minimum on a 20 kW-design house cycles whenever demand falls below 10 kW. A typical 2–4 bed house with about 12 kW design load "would cycle heavily". — [Heat Geeks](https://www.heatgeek.com/articles/this-will-make-you-think-again-about-how-efficient-your-brand-new-boiler-really-is)
- Hydronic vendor guidance: ideally 2–3 cycles per hour, with 5 or fewer per hour acceptable in practice. — [R.F. MacDonald, "Effects of ultra-high turndown in hydronic boilers"](https://www.rfmacdonald.com/boiler-white-papers/hydronic-boilers/effects-of-ultra-high-turndown-in-hydronic-boilers) (vendor)
- A vendor comparison argues that idling an ultra-high-turndown boiler can lose more energy than cycling a few times. — [Cleaver-Brooks, Ultra High Turndown](https://www.globalspec.com/CleaverBrooks/ref/UltraHighTurndown.pdf) (vendor, commercial scale)

**SAT's low-load scheme (from source)**
- PWM activates when overshoot protection is enabled and the boiler's minimum achievable flow setpoint (the overshoot protection value) is above the computed control setpoint. It is forced on if the boiler has no setpoint management. — [SAT climate.py `pulse_width_modulation_enabled`](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/climate.py)
- Duty fraction d = (requested_setpoint − base) / (boiler_flow_temp − base), clamped to 0–1, where base = 27.2 °C (radiators) or 20 °C (underfloor). — [SAT pwm.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/pwm.py)
- Timing:
  - Default limit is 4 cycles per hour, giving a 900 s nominal period. The minimum on-time is 180 s, and the low-duty off-time is stretched so that on-time stays 180 s. The maximum period is 2 × 900 s = 30 min.
  - Below half the lower threshold (d below about 10 %) the boiler stays off.
  - With automatic duty cycle disabled, a fixed period applies (default 13 min).
  - Cycle starts are refused once the hourly maximum is reached.

  — [SAT pwm.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/pwm.py); [const.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/const.py)
- During an ON phase with the flame on for more than 6 s, SAT sets the setpoint to (flow − 3 °C) to hold the boiler at minimum modulation. If flames are short (average under 60 s), it forces the setpoint to flow + 10 °C. The boiler deadband constant is 2 K and the heater start-up window is 180 s. — [SAT pwm.py, const.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/pwm.py)
- Overshoot protection calibration sends max modulation = 0 % and control setpoint = 75 °C, and finds the highest flow temperature the boiler reaches at 0 % modulation. It takes at least 20 minutes. Defaults are 62 °C for radiators, 45 °C for underfloor and 40 °C for heat pumps. If the boiler ignores 0 % modulation, an alternative calibration is used. — [SAT README](https://github.com/Alexwijn/SAT); [const.py `OVERSHOOT_PROTECTION_SETPOINT`](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/const.py)
- Experimental "dynamic minimum setpoint" adjusts the control setpoint using the boiler return temperature. The suggested adjustment factor is 0.1–0.5 (default 0.2). — [SAT README](https://github.com/Alexwijn/SAT)
- The README states that with automatic duty cycle, SAT computes on/off times in 15-minute intervals, extended "up to 30 minutes" in mild weather. — [SAT README](https://github.com/Alexwijn/SAT)

### Inferences
- With burners of 60–180 s, the UCL table implies a 1.5–4 % penalty, which is comparable to or larger than the whole compensation-control gain in the SAP table at 80/60. Avoiding short cycles therefore matters at least as much as the strategy choice.
- Practical rules for this firmware, derived from the above:
  - a minimum burner on-time of at least 3 min (preferably 5–10 min);
  - at most 3–4 CH starts per hour;
  - a minimum off-time;
  - a duty-cycle period of about 15–30 min when demand is below minimum output.
- Low-load lever ladder:
  1. Lower the flow setpoint toward the "minimum-output steady-state" temperature.
  2. Cap max modulation with ID 14 so the boiler ramps gently and its post-ignition high-fire burst is limited.
  3. Below the minimum, run a PWM of the flow setpoint (ON phase = setpoint just below the flow temperature to stay at minimum fire; OFF phase = setpoint 10 °C or CH-enable off).
  4. Increase system water volume (open the TRVs on the reference loop or a bypass radiator; the underfloor circuit adds mass) to lengthen cycles.
- Whether ID 14 = 0 % truly forces minimum fire varies by boiler, as SAT's fallback calibration shows. Verify on the actual boiler using ID 17 (relative modulation) feedback.

### Gaps
- No independent measured data on wear cost per start for domestic condensing boilers (ignition electrode, fan, heat-exchanger thermal stress). BRE found no study proving that TPI cycling shortens boiler life.
- No manufacturer-published "maximum starts per hour" for domestic boilers was retrieved. Built-in anti-cycle timers are often around 3–5 min, but this is not sourced here.

## Q4. Flow temperature capping, ΔT/return temperature and pump speed

### Takeaway
Cap the flow temperature as low as comfort allows and keep the return below about 55 °C, ideally well below it. A large ΔT (15–20 K for radiators) lowers the return for a given mean emitter temperature. Fixed high pump speeds narrow ΔT and raise the return.

### Cited Findings
- Compensation saves energy because the return temperature is lower at low demand. Some boilers have in-built modulating control that lowers output when the return temperature gets high. — [BRE 2017, §2.3, §5.2.1](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)
- SAT's maximum water setpoint guidance is 55–75 °C for radiators and 50 °C for underfloor. Its dynamic minimum setpoint uses the return temperature. — [SAT README](https://github.com/Alexwijn/SAT)
- Keep the flow temperature below 55 °C so the return stays under the dew point. — [Heat Geeks](https://www.heatgeek.com/articles/this-will-make-you-think-again-about-how-efficient-your-brand-new-boiler-really-is)

### Inferences
- Use ID 57 (MaxTSet) to cap the boiler maximum CH setpoint, and clamp the ID 1 output in firmware. Monitor ID 25 (flow) and ID 28 (return). A falling ΔT (flow − return) at steady state means too much pump flow (lower the pump speed or setting) or a bypass is open.
- For a 10 kW load at ΔT 20 K, the required flow is 10 / (4.19 × 20) ≈ 0.12 kg/s ≈ 7.2 L/min. At ΔT 10 K it is 14.3 L/min. These are simple physics, Q = ṁ·c_p·ΔT.

### Gaps
- No sourced data on optimal circulator settings (proportional versus constant pressure) for domestic condensing boilers, or on their measured effect on return temperature.

## Q5. OpenTherm-specific practice: ID 1 setpoint vs letting the boiler regulate, ID 14, ID 16/24, feedback IDs 17/25/28

### Takeaway
Most OpenTherm room controllers act as the master and write ID 1 (control setpoint, the CH flow temperature). Room setpoint and room temperature (IDs 16 and 24) are informational for most boilers. ID 14 (max relative modulation) is the second control lever. IDs 17, 25 and 28 give the feedback needed for anti-cycling and condensing logic.

### Cited Findings
- OpenTherm Data-IDs, from the widely used OpenTherm Arduino library header:

  | ID | Content | Type |
  |---|---|---|
  | 0 | master and slave status flags | flag8/flag8 |
  | 1 | TSet, CH water temperature setpoint | f8.8 °C |
  | 8 | TsetCH2 | |
  | 14 | MaxRelModLevelSetting | f8.8 % |
  | 15 | max boiler capacity kW / min modulation % | u8/u8 |
  | 16 | TrSet, room setpoint | |
  | 17 | RelModLevel | % |
  | 24 | Tr, room temperature | |
  | 25 | Tboiler, flow water temperature | |
  | 26 | Tdhw | |
  | 27 | Toutside | |
  | 28 | Tret, return water temperature | |
  | 48/49 | DHW and max CH setpoint bounds | s8/s8 |
  | 56 | TdhwSet | |
  | 57 | MaxTSet, max CH water setpoint | |

  — [ihormelnyk/opentherm_library OpenTherm.h](https://github.com/ihormelnyk/opentherm_library/blob/master/src/OpenTherm.h)
- BRE found that 26 % of advanced controls on the UK market use OpenTherm and 32 % use eBUS. — [BRE 2017, p. 43](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)
- SAT uses ID 14 as a control lever. It sets relative modulation to 0 % when not needed and to the configured maximum (default 100 %) otherwise, and uses MM = 0 with CS = 75 °C for overshoot calibration. — [SAT climate.py `relative_modulation_value`](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/climate.py); [SAT README](https://github.com/Alexwijn/SAT)

### Inferences
- If the boiler has its own weather curve and an outdoor sensor, the controller could send only CH-enable and let the boiler regulate. The degree to which a boiler uses ID 16/24 for its own room influence is vendor-specific. With an indoor-only sensor, computing ID 1 in the thermostat is the portable option.
- Read ID 15 once to learn the boiler's minimum modulation % and maximum kW. Minimum kW = min% × max kW, and this anchors the low-load PWM logic.

### Gaps
- The official OpenTherm Protocol v2.2/v4 PDF could not be fetched (HTTP 403), so the master communication-interval requirement (about 1 s, with the boiler falling back if no message arrives within a timeout) and the slave fallback behaviour are not verified here.
- No source found that quantifies how many boilers honour ID 16/24 for internal room compensation.

## Q6. Evidence of savings: OpenTherm modulation vs on/off

### Takeaway
The regulatory and modelled gains are about 2–3 percentage points over a Class I on/off thermostat (ErP), or 0.7–8 % depending on design flow temperature (SAP model). Test houses and chambers show 10–15 %, while the only large field trial (TPI) found no significant difference. A realistic expectation is a few percent, more if the system can run at 45/35-type temperatures.

### Cited Findings
- See the ErP class table, the SAP Table 2 and the evidence table in Q2. — [BRE 2017](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)
- Installer and manufacturer survey: 50 % expect more than 10 % savings from weather compensation. BRE regards this as expectation, not evidence. — [BRE 2017, §5.5](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)
- BRE break-even savings needed to repay the extra install cost: weather compensation (£200) 2.4–3.6 %, load compensation (£110) 1.3–2.0 %, TPI (£65) 0.8–1.2 %. — [BRE 2017, Table 8](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf)
- BEAMA/Salford 2021: load compensation with direct modulation gave the largest savings. — [HVP](https://www.hvpmag.co.uk/BEAMA-examines-energy-saving-potential-of-load-and-weather-comp-controls/12769)

### Inferences
- Because cycling losses (Q3) are large in oversized systems, a modulating controller that also enforces long burns will probably save more in practice than the steady-state SAP model suggests. This is not demonstrated in a fetched study.

### Gaps
- DESNZ/BEIS post-Boiler-Plus field evaluations and the Energy Saving Trust "in-situ monitoring of condensing boilers" (2009) were not retrieved.

## Q7. PID tuning for slow thermal systems

### Takeaway
Use a PI(D) controller on room error with a slow sample time (about 1 min), a deadband of about 0.1 K, integral clamping (anti-windup) tied to the feed-forward, and a heavily filtered derivative. SAT's defaults give a concrete, field-used starting point.

### Cited Findings
SAT's PID implementation and defaults (v3 controller, read from source):
- Defaults: sample time 1 min, deadband 0.1 K.
- The integral is updated only when |error| ≤ deadband (it is reset on entering the deadband) and is clamped to ± the heating-curve value. This conditional integration plus clamp is the anti-windup.
- The derivative is updated only when |error| > deadband and is filtered with two cascaded first-order filters (α1 = 0.8, α2 = 0.6). It acts on the error, not on the measurement.
- Output = P + I + D, added to the curve value.

— [SAT pid.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/pid.py); [const.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/const.py)

- Automatic gains (v3): Kp = coef × curve_value / 3 for radiators (/4 for underfloor), Ki = Kp/8400 per second, Kd = 0.07 × 8400 × Kp = 588·Kp (seconds). Example: coefficient 2 and a curve value of about 20 give Kp ≈ 13 °C flow per K room error, Ki ≈ 0.0016 °C/s per K (about 5.6 °C/h per K), Kd ≈ 7800 °C·s per K. Manual defaults are Kp 45, Ki 0, Kd 6000. — [SAT pid.py, const.py](https://github.com/Alexwijn/SAT/blob/master/custom_components/sat/pid.py)
- SAT recommends a room sensor with 0.01 °C resolution and a 30 s refresh, so the derivative and deadband work. — [SAT README](https://github.com/Alexwijn/SAT)

### Inferences
- Room-temperature dynamics are first order plus dead time, with time constants of hours (radiators) to many hours (underfloor). Integral times of hours (SAT: Ti = Kp/Ki = 8400 s ≈ 2.3 h) and derivative times of about 10 min (Td = Kd/Kp = 588 s) are consistent with this.
- Use derivative on measurement rather than on error to avoid kicks when the setpoint changes, as setpoint schedules do (standard PID practice, unsourced here). Freeze or back-calculate the integral whenever the output is saturated, PWM is active, the boiler is in DHW mode, or the flame is off for anti-cycling.
- Run the controller with separate gains or curves for the radiator and underfloor circuits only if they get separate setpoints. With a single boiler setpoint, tune for the radiators.

### Gaps
- No academic paper on domestic room-temperature PID tuning (e.g. SIMC or Ziegler–Nichols for buildings) was retrieved in this session.

## Q8. DHW with an external storage tank on a system boiler

### Takeaway
UK HSE L8 guidance requires storing hot water at 60 °C or more and distributing it so it reaches at least 50 °C within 1 minute (55 °C in healthcare premises). A periodic 60 °C+ cycle is common manufacturer practice but is not a stated HSE weekly requirement. Charging a cylinder coil with 70–80 °C flow pushes the return above the dew point late in the charge, so DHW charging largely does not condense.

### Cited Findings
- Store hot water at 60 °C or more. Distribute so it reaches 50 °C (55 °C in healthcare) within 1 minute at outlets. Check sentinel outlet and cylinder temperatures monthly. — [HSE, Legionnaires' disease: hot and cold water systems](https://hse.gov.uk/legionnaires/hot-and-cold.htm); [HSE healthcare legionella](https://hse.gov.uk/healthservices/legionella.htm)
- Manufacturers and installers mostly advise a weekly anti-legionella cycle that brings the whole cylinder to at least 60 °C. Some systems use 70 °C. Kill rates quoted: 90 % at 55 °C in about 20 minutes, 100 % over 5–6 hours. — [Heat Geeks, legionella and water temperature](https://heatgeek.com/articles/legionella-and-water-temperature-what-you-need-to-know); [Renewable Heating Hub forum](https://renewableheatinghub.co.uk/forums/domestic-hot-water-cylinders/legionnaires-disease-home-hot-water-understanding-risks-and-prevention/) (forum; lower quality)
- Heat Geeks also covers the scald/legionella trade-off for setpoint choice. — [Heat Geeks, hot water temperature, scalding and legionella](https://www.heatgeek.com/articles/hot-water-temperature-scalding-and-legionella)

### Inferences
- DHW setpoint guidance for this firmware:
  - a 50–55 °C tank (boiler ID 56 or tank thermostat) for daily use;
  - a reheat hysteresis of about 5–10 K, so the boiler does long charges rather than frequent short ones;
  - a weekly 60–65 °C cycle, timed to finish shortly before high draw, to limit standing losses.

  These are synthesised from the HSE and Heat Geeks sources, not from a single authoritative domestic rule.
- A 60 °C tank needs a coil flow of about 70–75 °C, so the return will be well above 55 °C for most of the charge. Charging at a lower flow temperature (e.g. 60–65 °C to a 50 °C tank) condenses for longer but takes longer and needs an adequately sized coil. On a system boiler, DHW priority (the CH pump or valve off while charging) temporarily stops space heating. Scheduling charges when space-heating demand is low, or right after a CH run when the water is already warm, reduces extra starts.

### Gaps
- No Hungarian legal requirement for domestic legionella pasteurisation was retrieved.
- No quantitative source on the efficiency cost of weekly pasteurisation, or on condensing fraction during coil charging.
- No primary source on the reheat hysteresis values that domestic manufacturers use.
