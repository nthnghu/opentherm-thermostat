# Learning, System Identification, Predictive Control, Efficiency Measurement and Safety for a DIY OpenTherm Thermostat (ESP32-P4 + Home Assistant)

Scope note: this covers a single-family house with a condensing gas boiler, one room sensor now (an SHT40) with more planned, and an optional outdoor sensor or weather forecast from Home Assistant (HA). "Inferences" bullets are the researcher's engineering judgement or general domain knowledge that was **not** verified against a source in this session. Report writers should present them as recommendations, not as cited fact.

## Q1. Grey-box RC building models: equations, estimation, data needs, accuracy

### Takeaway
A first-order (1R1C) or second-order (2R2C) lumped RC model, fitted by least squares or Kalman-filter maximum likelihood on about 3 weeks of 10-minute data, predicts room temperature with roughly 89% accuracy (100 − MAPE) in recent work. Adding more states rarely helps a single-zone house. DTU's CTSM-R and the Bacher & Madsen (2011) likelihood-ratio procedure are the reference methods for picking model order.

### Cited Findings
- Bacher & Madsen (2011), "Identifying suitable models for the heat dynamics of buildings", *Energy and Buildings* 43:1511–1522, DOI 10.1016/j.enbuild.2011.02.005. The paper proposes a forward-selection procedure over continuous-time stochastic grey-box (RC) models, using likelihood-ratio tests. A more complex model is kept only if it improves significantly on a simpler one. The method is demonstrated on DTU's FlexHouse. — [DTU Orbit](https://orbit.dtu.dk/en/publications/identifying-suitable-models-for-the-heat-dynamics-of-buildings/); [author PDF](https://02417.compute.dtu.dk/material/week13/article-grey-box-models.pdf)
- CTSM-R ("Continuous Time Stochastic Modelling in R") is a general framework for identifying and estimating partially observed continuous-discrete-time grey-box models: stochastic differential equations plus discrete measurements, estimated by maximum likelihood with an (extended) Kalman filter. — [ctsmr paper (arXiv 1606.00242)](https://ar5iv.arxiv.org/html/1606.00242)
- Shankar, Gaikwad & Dubey (WSU, arXiv 2510.21044, 2025), "House Thermal Model Estimation: Robustness Across Seasons and Setpoints":
  - **Models:** linear state space ẋ = A(θ)x + B(θ)u + D(θ)w, y = Cx with C = [1,0,…]. SM1 has 1 state (indoor air; ≈1R1C, 5 parameters: R_win, C_in, internal-gain, HVAC-gain and solar-gain coefficients). SM2 has 2 states (air + wall; ≈2R2C, 7 parameters). The 4-state "truth" model has 12 parameters. All are discretised with Euler's method.
  - **Estimators:** nonlinear least squares, batch estimation (process plus measurement noise) and Kalman-filter MLE, all solved in **CasADi + IPOPT**.
  - **Data:** 10-minute sampling and a **21-day training window** per season. The authors say longer windows did not help and caused convergence problems.
  - **Accuracy:** indoor-temperature accuracy (100 − MAPE) was LS 89.7%, BE 88.7% and MLE 88.4%. SM1 (89.0%) was as good as SM2 (88.8%). HVAC power was predicted less well (≈78–81%). Temperature accuracy stayed at 87–90% across seasons and at setpoints of 18–26 °C.
  - **Caveat:** the data was **simulated** (Pecan Street load data, NSRDB weather), not measured, and the paper does not report RMSE in °C. — [arXiv HTML](https://arxiv.org/html/2510.21044v1)
- A study of residential thermal-dynamics estimation reported that Bayesian optimisation estimated dynamic thermal characteristics with on average 31% higher accuracy than CTSM. The same study states that "a high volume of building sensor data is required". — [Aalborg Univ. / J. Building Eng. 2023 (PDF)](https://vbn.aau.dk/files/539411538/1-s2.0-S2352710222016898-main.pdf) (from the search snippet; the full text was not read)
- A second-order thermal-network study of residential buildings (OSTI) reported that "a modest amount of training data" gave reliable parameter estimates and accurate prediction. — [OSTI record 1842207](https://bvbvf1.bib-bvb.de/vufind/PrimoRecord/cdi_osti_scitechconnect_1842207) (from the search snippet)
- A CTSM-R office-zone study lists sensor accuracies of indoor ±0.15 °C and outdoor ±1 °C. It examined dataset size, number of states and prediction horizon as variables. — [E3S Conf. HVAC 2021, 11006](https://www.e3s-conferences.org/articles/e3sconf/pdf/2021/22/e3sconf_hvac2021_11006.pdf) (from the search snippet)

### Inferences
- **Recommended starting model for this house (not verified against a source):**
  - 1R1C: C·dT_in/dt = (T_out − T_in)/R + η·Q_boiler + A_sol·I_sol + Q_int.
  - Q_boiler can be estimated from OpenTherm relative modulation × max capacity, or better from the gas meter.
  - Upgrade to 2R2C (air node + thermal-mass/emitter node) when the heat-up curve shows a fast step followed by a slow tail. That shape is typical for radiators and especially underfloor heating.
  - The 1R1C time constant τ = R·C is the single most useful learned quantity: it drives optimum start and setback decisions.
- **Discrete ARX form for an MCU:** T[k+1] = a·T[k] + b1·T_out[k] + b2·u[k] + b3·I_sol[k] + c. This is linear in its parameters, so it can be fitted by ordinary least squares or **recursive least squares (RLS) with a forgetting factor** on the ESP32-P4 at negligible cost (a 4×4 or 5×5 matrix update every 5–10 min). The physical R and C can be recovered from a and b.
- **Identifiability pitfalls:**
  - With constant thermostat operation and one room sensor, R and C are poorly identified, because the feedback loop keeps T_in nearly constant.
  - Night setbacks, step changes in setpoint and free-floating cool-down periods (boiler off) give far better excitation.
  - Solar gains and internal gains (people, cooking) are the main unmodelled disturbances.
  - Without an irradiance signal, a cloud-cover forecast or the PV production (if any) can serve as a proxy.
- **Data needed:** the findings point to about 2–4 weeks of heating-season data at 5–10 min resolution for a usable 1R1C or 2R2C fit, re-fitted periodically (seasonal drift, ventilation habits). A full heating season is needed to validate the heating curve at design outdoor temperatures.

### Gaps
- I did not retrieve RMSE (°C) values from measured, real-house studies; the WSU accuracy figures come from simulation. The full Bacher & Madsen and CTSM-R texts were not read in detail, so specific residual RMSE numbers (often quoted as about 0.1–0.3 °C for one-step-ahead prediction) are unverified.
- IEA EBC Annex 58 / Annex 71 reports on in-situ characterisation (co-heating, dynamic RC identification) were not retrieved in this session.

## Q2. Applications: adaptive heating curve, optimum start, forecast feed-forward, MPC, RL, commercial products

### Takeaway
For a modulating condensing boiler, most of the realistic gain comes from:
- weather or load compensation (low flow temperature plus modulation instead of on/off), worth about 10–12% gas in the BEAMA/Salford tests;
- avoiding short-cycling.

Learning features (optimum start, an adaptive curve) add comfort and a few percent. Field MPC reviews report about 16–22% average savings in residential buildings, but 71% of field studies use unreliable protocols and none of the reviewed residential examples was clearly a gas boiler. RL is research-grade overkill at homeowner scale.

### Cited Findings
- **Load and weather compensation (UK, BEAMA-led Salford University tests, 2021):**
  - Replacing a standard room thermostat with a proportional on/off load-compensating thermostat reduced gas use by about **10%** over the heating season.
  - A directly modulating (OpenTherm-type) load- or weather-compensating thermostat reduced it by about **12%**.
  - Load compensation with direct modulation gave the largest savings, and weather compensation with direct modulation was judged similar. — [BEAMA press release](https://www.beama.org.uk/news/the-heat-is-on-beama-led-research-reveals-heating-controls-can-reduce-gas-use-by-10-12-percent-amid-record-price-hikes.html); [BEAMA test report page](https://beama.org.uk/resourceLibrary/salford-tests-on-load-and-weather-compensation-.html). Note: industry-sponsored.
- BRE produced a client report for BEIS (Sept 2017) collating evidence on weather compensation, load compensation and TPI. — [gov.uk BRE report PDF](https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf); [gov.uk page](https://gov.uk/government/publications/heating-controls-compensation-and-time-proportional-and-integral-tpi-controls). The PDF is a scanned image, and its numbers could not be extracted in this session.
- A BEIS international review found "limited international evidence, high quality or otherwise" on the savings from heating controls. — [BEIS heating controls international evidence](https://assets.publishing.service.gov.uk/government/uploads/system/uploads/attachment_data/file/650840/heating-controls-international-evidence.pdf)
- **Older outdoor-reset evidence:** a 1988 ACEEE paper cites 10–26% annual space-heating savings from outdoor reset plus cut-out controls on boilers (North America). — [ACEEE 1988](https://www.aceee.org/files/proceedings/1988/data/papers/1988_V2_005.pdf)
- **MPC/RL field-demonstration review:** Khabbazi, Pergantis, Reyes Premer, Papageorgiou, Lee, Braun, Henze & Kircher (2025), "Lessons learned from field demonstrations of MPC and RL for residential and commercial HVAC: A review" (arXiv 2503.05022). — [arXiv](https://arxiv.org/html/2503.05022v6)
  - **Coverage:** 24 residential papers and 52 residential field tests.
  - **Savings:** after reliability filtering, 25 tests from 9 papers remain. Their duration-weighted savings average **22% energy and 16% cost**.
  - **Reliability:** 71% of all field demos use protocols that may give unreliable estimates.
  - **What inflates savings:** simulation baselines overestimate savings because model prediction error (comparable to the 10–20% savings claimed) is folded in. Tests shorter than 7 days also overestimate savings, partly because of thermal-mass effects; unfiltered tests under one week show 57% cost savings versus 22% for longer tests. Controlling only a subset of rooms inflates savings as well.
  - **Statistics:** only 3 of 23 residential papers reported confidence intervals.
  - **Cost/complexity:** one residential MPC deployment (Pergantis et al.) took **190 engineer-days** (150 one-time and 40 recurring). Knudsen et al. stress periodic model recalibration as an ongoing cost. A low-cost plug-and-play system of about $50 exists (Wang et al.). Only 13 of 104 papers report deployment or maintenance costs.
- A US patent describes predictive control of a domestic gas boiler that tunes its model with gas-meter data, but it reports no field results. — [USPTO 12169072](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/12169072)
- Residential MPC field test (Energy and Buildings 2023, apartment, **cooling**): 22.1% (bedroom) and 26.8% (living room) cost savings over rule-based control in a 1-month test. This was not a gas boiler. — [structurae listing](https://structurae.info/fr/litterature/article-de-revue/field-test-of-model-predictive-control-in-residential-buildings-for-utility-cost-savings)
- **Commercial products:**
  - **Nest True Radiant** "learns how long your home takes to heat up". It turns heating on early to reach the target on time and off early to avoid overshoot, with a user-capped "Max Duration". Forced-air systems use "Early-On" instead. — [Google Nest Help: True Radiant](https://support.google.com/googlenest/answer/9250165)
  - **tado Early Start** is "designed for slow heating systems, like underfloor heating". Community reports describe pre-heat starts of about 3 h and mixed learning quality. — [tado support](https://support.tado.com/en/articles/3478195); [tado community](https://community.tado.com/en-gb/discussion/12382/introduce-a-learning-ability-to-early-start-for-radiators)
  - **tado "heating curve optimisation"** (29 Jan 2025, heat pumps only): the user picks one of 5 presets (Comfort … Eco Plus). The release describes no automatic learning and gives no savings figure. — [tado press](https://www.tado.com/en/press/heat-curve-optimisation-feature-launch)
  - **Nest ErP classification:** Class IV (TPI, +2%) on on/off boilers and Class VI (weather compensator + room sensor, modulating, +4%) when connected via OpenTherm. — [Nest ErP sheet (PDF)](https://nest.com/support/images/misc-nest-thermostat-eu/064-00025-GB-A_A2_ErP_English.pdf)
- **DIY/HA reference project, SAT (Smart Autotune Thermostat)** — [GitHub Alexwijn/SAT](https://github.com/Alexwijn/SAT). It is an HA custom component driving OpenTherm (OTGW, **DIYLess shield**, ESPHome OpenTherm).
  - **Control:** an outdoor-temperature heating curve (Classic/Quantum/Precision variants), PID (Classic/Improved/Adaptive) with automatic gains that scale with the heating-curve value, and roughly 20-minute automatic gain calibration.
  - **Low-load handling:** "overshoot protection value" (OPV) calibration finds the flow temperature the boiler reaches at 0% modulation. When demand is below minimum boiler output, it switches to duty-cycle/PWM operation in 15–30 min periods.
  - **Limits and safety:** maximum setpoint suggestions of 55–75 °C for radiators and 50 °C for underfloor, plus window and contact sensors. A physical primary thermostat can act as backup if HA fails.
  - **Inputs:** it recommends an inside sensor with 2 decimals refreshed every 30 s. It has no explicit building model.

### Inferences
- **Priority ladder (cost/benefit for this project, unverified ranking):**
  1. Correct weather-compensated flow temperature with room-temperature feedback (parallel shift of the curve) and modulation. This is ErP Class VI, the biggest known win.
  2. Anti-short-cycling and low-load handling, the SAT-style OPV/PWM approach.
  3. Optimum start / optimum stop from a learned 1R1C time constant: learn °C/h heat-up rate as a function of (T_set − T_in) and T_out.
  4. Adaptive heating-curve slope and offset: nudge them slowly from the steady-state integral term, for example if PI integral output stays positive for days at a given T_out, raise the curve at that T_out.
  5. Forecast feed-forward: use the HA weather forecast for T_out over the next 3–6 h, plus solar/cloud cover to reduce flow temperature ahead of sunny periods. This matters mostly for high-mass underfloor heating.
  6. Full MPC.
- MPC benefits in the literature come largely from load shifting under time-of-use prices and pre-cooling. With flat gas tariffs and one zone, the remaining MPC benefit over a well-tuned weather-compensated PI plus optimum start is likely small (single-digit %), and it is hard to prove given the measurement-noise issues the review identifies.
- **RL is overkill:**
  - It needs either months of exploration on the real house (comfort risk) or a simulator, which means first building the RC model.
  - The RC model plus classical control already captures most of the value.
  - The review shows that RL field results are scarce and unreliable.

### Gaps
- I found no public field study of MPC specifically on a residential **condensing gas boiler** with measured savings.
- The Vaillant sensoCOMFORT adaptive heating curve was not documented in the sources found.
- The BRE/BEIS 2017 report's numeric findings could not be read (scanned PDF).

## Q3. Feasibility on the MCU vs offloading to Home Assistant / Python

### Takeaway
Run safety logic, PI control, the heating curve, anti-cycling, optimum start and a recursive-least-squares 1R1C/ARX model on the ESP32-P4. Run batch model fitting (multi-week grey-box/Kalman MLE), forecast-driven optimisation and any MPC in Python on the HA server, which writes back only setpoint or curve parameters. The thermostat must keep running safely when HA is absent.

### Cited Findings
- Residential grey-box identification in current research is done with CasADi + IPOPT (NLS, batch estimation, Kalman MLE) on 21 days of 10-minute data, which is ≈3,000 samples per signal. — [arXiv 2510.21044](https://arxiv.org/html/2510.21044v1)
- CTSM-R is an R package (with a C/Fortran core) for continuous-discrete stochastic grey-box estimation. It is a desktop/server tool. — [ctsmr paper](https://ar5iv.arxiv.org/html/1606.00242)
- The SAT HA integration shows the "HA does the control" architecture: an HA custom component does the curve, PID and PWM, while the OpenTherm hardware is a dumb gateway. It explicitly recommends a physical primary thermostat as a backup in case HA fails. — [GitHub Alexwijn/SAT](https://github.com/Alexwijn/SAT)
- MPC deployments carry heavy engineering and recalibration effort (190 engineer-days in one residential deployment). — [arXiv 2503.05022](https://arxiv.org/html/2503.05022v6)

### Inferences
- **Computational cost:**
  - RLS on a 4–6 parameter ARX model costs O(n²) per step: tens of floating-point operations every 5–10 min. That is trivial on the ESP32-P4 (dual-core RISC-V, about 400 MHz, with a hardware FPU).
  - A small linear MPC over a 24–48 step horizon with box constraints could even run on the P4 as a dense QP solver or with a simple shooting method. Writing and debugging that in C is a large effort, though.
  - In Python the stack would be:
    - model fitting: scipy.optimize.least_squares, or a hand-written Kalman filter MLE with CasADi;
    - optimisation: CasADi or cvxpy (OSQP);
    - forecasting (optional; overkill): darts/statsmodels.
- **Recommended split:**
  - The MCU owns hard limits, the watchdog, fallback behaviour, the OpenTherm bus timing, PI plus heating curve, and RLS-based heat-up-rate learning. Parameters are persisted in NVS with sanity bounds.
  - HA/Python owns offline re-fitting (weekly), KPI computation, A/B scheduling, and optional forecast feed-forward. It pushes bounded parameters to the MCU, for example curve slope/offset and τ, with **range checks done on the MCU**.
- Tiny ML models (TFLite Micro) on the ESP32 add nothing over a physics model here: the data is low-dimensional and the physics is known.

### Gaps
- I found no benchmark of an MPC/QP solver specifically on the ESP32-P4.
- No public HA integration that does grey-box RC identification for boilers was found in this session. There are community projects for heat pumps, but they were not verified.

## Q4. Data logging schema from day one

### Takeaway
Log everything OpenTherm exposes, plus room, outdoor and gas-meter data, from the first day the boiler is connected. Use ≤1 min resolution for fast boiler signals, event logging for burner starts, and long-term retention outside HA's default 10-day recorder. Without a pre-change baseline and a gas meter reading, no efficiency claim can be verified later.

### Cited Findings
- Field-demonstration reliability depends on measurement-based baselines in the same building at different times, with weather normalisation, and on tests of at least 7 days. That makes a pre-change baseline log essential. — [arXiv 2503.05022](https://arxiv.org/html/2503.05022v6)
- Residential grey-box fitting in the literature uses 10-minute data. — [arXiv 2510.21044](https://arxiv.org/html/2510.21044v1)
- SAT recommends an inside temperature with 2 decimals, refreshed every 30 s, for control. — [GitHub Alexwijn/SAT](https://github.com/Alexwijn/SAT)
- In the DTU low-cost MPC example, the instrumentation was a data logger (≈$2,800), a flow meter and a smart power meter (each ≈$200+); a $50 plug-and-play setup also worked. This shows that energy metering is the expensive part. — [arXiv 2503.05022](https://arxiv.org/html/2503.05022v6)

### Inferences
Recommended schema. These are the researcher's recommendations; the OpenTherm data IDs are from general knowledge of the OpenTherm 2.2 spec and should be verified against the spec.

| Signal | Source | Rate / type | Notes |
|---|---|---|---|
| Room temp, humidity (per room) | SHT40 now, more later | 30–60 s raw; 5 min mean | Keep raw resolution (0.01 °C); log sensor ID and calibration offset |
| Room setpoint, mode (comfort/eco/away/manual) | thermostat | on change | Needed to separate user-driven vs control-driven changes |
| Control setpoint sent (TSet, ID 1) | thermostat | every write | Also log the controller's internal terms (curve value, P, I, feed-forward) |
| Flow temp (ID 25), return temp (ID 28) | boiler | 10–30 s | Return temp drives the condensing KPI; not all boilers report ID 28 |
| Relative modulation (ID 17) | boiler | 10–30 s | Basis for an energy estimate |
| Status flags (ID 0): flame, CH active, DHW active, fault | boiler | 10 s and on change | Flame on/off **edges** give burner starts and run lengths |
| Fault/OEM codes (ID 5, 115) | boiler | on change | |
| CH water pressure (ID 18) | boiler | 1–5 min | Slow leak detection; low-pressure alarm |
| Max capacity / min modulation (ID 15), max CH setpoint (ID 57) | boiler | at boot | Needed for the kW estimate |
| Boiler counters (IDs 116–123: burner starts, CH pump starts, burner hours, ...) | boiler | hourly | Cross-check for edge-counted starts if the boiler supports them |
| Boiler-reported outside temp (ID 27), DHW temp (ID 26) | boiler | 1–5 min | If an outdoor sensor is fitted to the boiler |
| Outdoor temp | HA weather / own sensor | 5–15 min | Log both the forecast and the "actual" value; note the source |
| Solar irradiance / cloud cover | HA weather, PV inverter | 5–15 min | PV power is a cheap irradiance proxy |
| Gas meter | pulse reader / smart meter (P1/DSMR, HAN) | pulse events, or 5 min–hourly | Ground truth for energy; split CH vs DHW using DHW flags |
| Window open, occupancy/presence | HA contact sensors, phone presence, camera motion | on change | Major disturbance terms for the model |
| Wi-Fi RSSI, OT comm errors, reboots, reset reason, firmware version | MCU | on change / 5 min | Data-quality and reliability KPIs |

- **Storage:**
  - HA recorder (SQLite by default) keeps detailed states for a limited period (purge_keep_days, default 10). Long-term statistics keep only hourly mean/min/max for sensors that declare a state_class.
  - For model fitting at 10–60 s resolution, use InfluxDB or VictoriaMetrics alongside HA, or have the MCU write daily CSV/Parquet to SD card or flash with an upload.
  - An on-device ring buffer (a few days at 30 s ≈ tens of kB per signal) covers HA or Wi-Fi outages.
  - The HA recorder details above are general knowledge, not verified in this session.
- **Data-quality concerns:**
  - Timestamps: NTP sync; log in UTC.
  - Sensor self-heating: the SHT40 sits near the display and ESP32, so the board's own heat biases readings. Calibrate against a reference and log the offset.
  - Missing data versus a real zero (flame off).
  - Mode changes, including DHW draws, which distort CH energy.
  - Firmware-version changes, needed to split A/B periods.
  - Units and scaling of the OpenTherm f8.8 format.
  - Deduplication: HA only records on change, so a sensor that sits flat produces sparse rows that must be forward-filled carefully.

### Gaps
- I did not verify the OpenTherm data-ID list or the HA recorder defaults against primary docs in this session. Check the OpenTherm Protocol Specification v2.2/v4.2 and the HA recorder documentation.

## Q5. Efficiency metrics, KPIs and A/B testing

### Takeaway
The KPIs that matter for a condensing boiler are:
- gas per heating-degree-day (weather-normalised consumption);
- burner starts per hour or day, and average run length (cycling);
- percentage of burner-on time with return temperature below about 55 °C, the condensing threshold;
- average modulation level.

A gas meter is the ground truth. A modulation × capacity estimate is a useful but biased proxy. A/B comparisons need alternating multi-day blocks with degree-day normalisation; tests shorter than a week, or tests against simulated baselines, are known to inflate savings.

### Cited Findings
- **Condensing threshold:** flue-gas water vapour begins condensing when return water entering the boiler is below about 130 °F (≈54–55 °C). Another source places the flue-gas dew point at 127 °F (≈53 °C) at about 30% excess air. One manufacturer-style example gives **94% efficiency condensing vs 88% non-condensing**. — [Deppmann, "Tis the Season for Condensing Boilers" (PDF)](https://www.deppmann.com/wp-content/uploads/2016/10/RLD_MMM_%E2%80%98Tis-the-Season-for-Condensing-Boilers.pdf); [ACHR News, Condensing boilers: maximizing benefits](https://www.achrnews.com/articles/157465-condensing-boilers-maximizing-benefits) (from search snippets; trade press)
- **Cycling rules of thumb:**
  - Boiler systems should be designed to cycle no more than 2–3 times per hour, and ≤5/h is considered acceptable. Trade press calls more than 6/h short-cycling. — [Cleaver-Brooks, Ultra-High Turndown (PDF)](https://www.globalspec.com/CleaverBrooks/ref/UltraHighTurndown.pdf); [RF MacDonald, Effects of Ultra-High Turndown](https://www.rfmacdonald.com/boiler-white-papers/hydronic-boilers/effects-of-ultra-high-turndown-in-hydronic-boilers); [PM Mag, Is boiler turndown really that important](https://www.pmmag.com/articles/103620-is-boiler-turndown-really-that-important) (vendor/trade sources; commercial-boiler context)
  - Practitioner forums suggest a ~10-minute minimum run so the return reaches condensing temperature by the end of each run. — [HeatingHelp forum](https://forum.heatinghelp.com/discussion/comment/1318212/) (anecdotal)
- **Start/stop cycling in UK homes:** a UCL paper on boiler start/stop behaviour analysed domestic gas-boiler cycling. — [UCL Discovery (PDF)](https://discovery-pp.ucl.ac.uk/id/eprint/10054754/1/StartStop_BG_Paper_BSERT_v4_final_main_updated_figures_references.pdf). The fetch was blocked (HTTP 403), so the numbers are not extracted.
- **Oversizing and cycling:** a DOE/NREL Building America field test of a boiler load-monitoring controller found savings depended on how oversized the boiler was. Oversized boilers saw cycling and energy reductions consistent with lab studies; less-oversized boilers saw muted savings. The vendor claimed 10–30%. — [DOE Building America case study (PDF)](https://www.energy.gov/sites/prod/files/2014/11/f19/ba_case_study_boiler_load_monitoring_controls.pdf); [NREL record](https://research-hub.nrel.gov/en/publications/field-test-of-boiler-primary-loop-temperature-controller/)
- **A/B methodology:**
  - Measurement-based baselines (same building, different times, weather-normalised) are preferred over simulation baselines.
  - Tests under 7 days overestimate savings because of thermal mass.
  - Report confidence intervals; most residential studies don't.
  - Model prediction errors are comparable to the 10–20% savings typically claimed, so small effects need long tests. — [arXiv 2503.05022](https://arxiv.org/html/2503.05022v6)

### Inferences
- **KPI definitions (recommended):**
  - **HDD-normalised consumption:**
    - Daily gas_CH (m³ or kWh, with DHW removed) regressed on HDD = max(0, T_base − T_out,daily mean), with T_base around 15.5 °C (UK) or about 15–18 °C. Better still, fit T_base from the data with a change-point "energy signature" regression: E_day = a + b·HDD(T_base).
    - The slope b (kWh/K·day) is the house's heat-loss coefficient divided by efficiency. Compare b between strategies, not raw kWh.
    - Include indoor temperature in the normalisation (use T_in − T_out degree-days) when strategies change the comfort temperature, otherwise "savings" are just a colder house.
  - **Cycling:** burner starts per day and per burner-hour, median run length, and % of runs shorter than 10 min.
  - **Condensing:** % of flame-on time with T_return < 55 °C (stricter: < 50 °C), plus flame-on-weighted mean return temperature.
  - **Modulation:** mean relative modulation while the flame is on, and % of time at minimum modulation (a sign the boiler is oversized for the load).
  - **Comfort:** degree-hours below setpoint − 0.5 K during occupied periods, overshoot degree-hours, and RMS tracking error. These guard against "savings" that cost comfort.
- **Energy estimate from modulation:**
  - P ≈ P_min + (RelMod/100)·(P_max − P_min). Whether OT ID 17 is scaled relative to minimum or maximum output is boiler-specific.
  - Calibrate by regressing hourly gas-meter energy on Σ(flame-on × estimated P).
  - Expect errors of ±10–30% uncalibrated: RelMod semantics differ by manufacturer, DHW and CH get mixed, and ignition/pre-purge losses are not captured.
  - A pulse reader (reed switch or optical on the gas meter, typically 0.01 m³/pulse) or the smart meter's HAN/P1 port gives ground truth. 1 m³ natural gas ≈ 10–11 kWh (HHV), depending on the gas.
- **A/B protocol:**
  - Alternate strategies in blocks of at least 7 days, ideally 2 weeks; or use randomised day-pairs only for strategies with short memory.
  - Run at least 4–6 switches over a heating season.
  - Analyse with the energy-signature regression plus a strategy dummy variable to get a CI on the savings.
  - Discard the first 1–2 days after each switch (thermal mass carry-over).
  - Keep comfort setpoints identical between arms.
  - Log the firmware/strategy ID in every record.

### Gaps
- I found no sourced curve linking starts per hour directly to seasonal efficiency for a domestic condensing boiler; the UCL start/stop paper was inaccessible.
- The ±10–30% error estimate for modulation-based energy estimation is the researcher's judgement, not a cited figure.

## Q6. Safety and reliability design for a DIY OpenTherm thermostat

### Takeaway
The boiler keeps its own certified safety chain: flame supervision, high-limit, flow-temperature and pressure protection. That chain is independent of the thermostat. The DIY controller is a Class A (non-safety) control under EN 60730 Annex H, *provided* it never acts as the only protection against a hazard. Underfloor heating therefore needs an independent hardware high-limit thermostat or mixing valve, never firmware alone.

The design goals are:
- fail toward "boiler falls back to its own default / frost protection", not "boiler runs flat out";
- clamp every value sent to the boiler;
- rate-limit writes;
- make OTA updates atomic, with rollback.

With OpenTherm modulation plus weather compensation and a room sensor, the system meets ErP Class VI (+4%). Class VIII (+5%) needs 3 or more room sensors.

### Cited Findings
- **ErP temperature-control classes (Reg. EU 811/2013, Commission communication 2014/C 207/02):**
  - Class I: on/off room thermostat (1%).
  - Class II: weather compensator for modulating heaters (2%).
  - Class III: weather compensator for on/off heaters (1.5%).
  - Class IV: TPI room thermostat for on/off heaters (2%).
  - Class V: modulating room thermostat for modulating heaters (3%).
  - Class VI: weather compensator + room sensor, modulating (4%). It "varies the flow temperature … dependent upon prevailing outside temperature and selected weather compensation curve. A room temperature sensor monitors room temperature and adjusts the compensation curve parallel displacement … Control is achieved by modulating the output of the heater."
  - Class VII: weather compensator + room sensor, on/off heaters (3.5%).
  - Class VIII: multi-sensor room temperature control for modulating heaters, "equipped with 3 or more room sensors", varying flow temperature from the aggregated room deviations (5%). — [Nest ErP sheet (IV, VI text)](https://nest.com/support/images/misc-nest-thermostat-eu/064-00025-GB-A_A2_ErP_English.pdf); [Worcester Bosch FAQ](https://www.worcester-bosch.co.uk/professional/support/troubleshooting/faqs/erp-faqs/how-do-controls-affect-erp); [BEAMA guidance (docx)](https://www.beama.org.uk/static/uploaded/39230df3-fd53-401b-838205892bb97276.docx); [EU energy-efficient-products: space heater packages](https://energy-efficient-products.ec.europa.eu/product-list/space-heaters/packages-space-heaters_en); [Reg. 811/2013 annexes](https://legislation.gov.uk/eur/2013/811/annexes/adopted)
  - The I–V percentages come from a German-language table seen as a garbled search snippet ([meinhausshop ErP PDF](https://www.meinhausshop.de/media/pdf/61_016_26--ErP-01-fr.pdf)). The VI, VII and VIII percentages are confirmed by multiple sources. Verify I–V against the EUR-Lex text.
- Nest is rated Class VI (+4%) on OpenTherm and Class IV (+2%) on on/off boilers. — [Nest ErP sheet](https://nest.com/support/images/misc-nest-thermostat-eu/064-00025-GB-A_A2_ErP_English.pdf)
- **EN/IEC 60730-1 Annex H software classes:** — [Microchip: Software Classes](https://onlinedocs.microchip.com/oxy/GUID-A619C762-0FA4-4AA9-B164-91996365FAF7-en-US-9/GUID-24711A10-AEB3-402F-8B75-B35422232E0F.html); [Intertek blog 2026](https://www.intertek.com/blog/2026/05-22-functional-safety-and-iecul-60730-1-annex-h/)
  - Class A: control functions "not intended to be relied upon for the safety of the equipment".
  - Class B: software intended to prevent hazards if a non-software fault occurs, for example thermal cut-outs. It requires single-fault tolerance and self-tests (RAM/ROM/CPU/clock).
  - Class C: prevents special hazards without other protective devices, for example automatic gas burner controls.
  - The current edition is IEC 60730-1 Ed. 6.0 (2022-09); EN 60730-1:2016 is listed as superseded. — [VDE preview](https://www.vde-verlag.de/iec-normen/preview-pdf/info_iec60730-1{ed6.0}b.pdf); [NSAI shop](https://shop.standards.ie/en-ie/standards/i-s-en-60730-1-2016-871855_saig_nsai_nsai_2073033)
- **Comm-loss behaviour (boiler/thermostat side):**
  - A Remeha iBase base station flags a fault when "there is no OpenTherm communication for 60 seconds". — [Remeha iBase manual](https://www.manualslib.de/manual/526801/Remeha-Ibase.html?page=25)
  - A forum user reports a boiler showing an error after about 15 s with no room unit on the OT bus. — [domoticaforum](https://domoticaforum.eu/viewtopic.php?p=87767) (anecdotal)
  - A thread documents a "boiler runaway when removing OTGW power" case: unexpected boiler behaviour when an in-line gateway loses power. — [domoticaforum: Boiler runaway when removing OTGW power](https://www.domoticaforum.eu/viewtopic.php?p=100938) (anecdotal; not read in full)
- **HA-dependence risk:** SAT recommends a physical primary thermostat as a backup in case Home Assistant fails. It suggests maximum flow setpoints of 55–75 °C for radiators and **50 °C for underfloor**, plus an overshoot-protection value and a maximum modulation cap. — [GitHub Alexwijn/SAT](https://github.com/Alexwijn/SAT)

### Inferences
Design checklist (the researcher's recommendations from general embedded/heating practice; not individually sourced):

- **Layered protection:**
  - Layer 1 is the boiler's own certified controls: flame failure, overheat/high-limit (typically around 90–110 °C), low pressure, frost protection. These are typically built into the boiler and active regardless of the thermostat.
  - Layer 2 is the independent hardware for underfloor: a mechanical high-limit thermostat (typically 50–55 °C) on the UFH flow that cuts the UFH pump or zone valve, plus a thermostatic mixing valve.
  - Layer 3 is the firmware clamps.
  - Layer 4 is HA/remote logic.
  - Never rely on layer 3 or 4 for screed or pipe protection.
- **Boiler behaviour on comm loss:**
  - The OpenTherm spec expects the master to send a frame roughly every second, and slaves treat a long silence as a lost connection.
  - Many boilers then revert to their own room-thermostat on/off terminals or default CH setpoint, or show a fault. What a particular boiler does varies by model and **must be tested on the actual boiler**: pull the OT cable and power down the MCU, then watch what the boiler does.
- **Watchdog:**
  - Enable the ESP-IDF task watchdog on the control and OT tasks, plus the interrupt watchdog.
  - Feed the watchdog only from the main control loop after a successful cycle.
  - On reset, boot into a conservative default: last-known-good setpoint clamped, or a frost-protection-only mode.
  - Log the reset reason.
- **Sensor failure:**
  - Detect implausible readings: out of range (< 0 °C or > 40 °C indoors), stuck value for N minutes while the boiler runs, rate of change > X K/min, I²C NACK, or CRC failure (the SHT40 has a CRC).
  - On failure, fall back first to a secondary sensor (HA-provided), then to pure weather compensation from the outdoor temperature, then to a fixed moderate flow temperature (e.g. 40–45 °C for radiators) with a duty limit.
  - Raise an alert.
  - Never "assume cold and run flat out".
- **Frost protection:** keep it independent of schedules and HA. If T_room < 5–7 °C, or the boiler-reported or outdoor temperature implies freezing risk, enable CH at a low setpoint regardless of mode. Most boilers also have a built-in frost function triggered by low water temperature.
- **Clamps:**
  - TSet ∈ [boiler min (~20–30 °C), configured max]. For underfloor, configured max ≤ 45–50 °C; for radiators, ≤ 70–75 °C.
  - Also write the OT "max CH setpoint" (ID 57) and "max relative modulation" (ID 14) as a second line, where the boiler supports them.
  - Validate any parameter pushed from HA on the MCU, rejecting out-of-range curve slopes and offsets.
- **Rate-limit writes:**
  - Write only on meaningful change (e.g. ≥0.5 K), at most every 1–2 min for TSet changes. Keep the continuous OT polling cadence, since that is reading, not writing.
  - Persist to flash (NVS) only on change, with debounce, to avoid flash wear.
  - Some boilers write certain OT parameters (ID 56/57 remote parameters) to EEPROM, so avoid writing them repeatedly.
- **Minimum burner on/off times (anti-cycling):** for example a minimum 10 min on and 10 min off where possible, using the boiler's own anti-cycle timer as backup.
- **OTA:**
  - Use ESP-IDF dual OTA partitions with app rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE). Mark the new image valid only after self-test passes: OT communication OK and sensor OK for N minutes.
  - Sign the images.
  - Don't OTA during active heating demand in very cold weather.
  - The control must keep running while the download happens.
- **Communication and HA loss:** the MCU must run autonomously with the last schedule and curve stored locally. HA is advisory. If HA is unreachable, keep local control. If OT is lost, retry, alarm, and rely on the boiler's fallback.
- **Pressure/fault monitoring:** alarm on low CH pressure (e.g. < 1.0 bar), on boiler fault flags, and on flow − return ΔT anomalies (pump failure).
- **Standards positioning:**
  - A DIY room controller that only sends setpoints, with all hazard protection in the certified boiler and the hardware UFH limit, is functionally Class A software.
  - Claiming an ErP class formally requires the manufacturer's declaration; for a DIY device it is only a reference for what the control strategy achieves.
  - **Class VI** needs weather compensation, a room sensor adjusting the curve, and OT modulation. **Class VIII** becomes available once 3 or more room sensors aggregate into the flow-temperature calculation, matching the planned "more sensors later".
- **Electrical:** the OpenTherm interface is low voltage, and the DIYLESS shield isolates it. Mains wiring of any backup relay or room-stat terminals should follow local regulations.

### Gaps
- I did not retrieve the exact OpenTherm-spec master-timeout and slave-fallback values. I found no authoritative statement of what boilers do on OT loss; it is manufacturer-specific and anecdotal.
- I did not read the EN 60730-2-9 (temperature sensing controls) specifics for room thermostats.
- Verify the ErP class percentages for I–V against the official EUR-Lex text of 2014/C 207/02.
- I found no sourced data on DIY-thermostat failure incidents.
