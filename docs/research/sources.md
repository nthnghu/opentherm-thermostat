# Sources

[← Index](README.md)

Every factual claim in the chapters links to its source inline. The [full report](full-report.md) and the [research notes](notes/) carry the complete citation trail, including which items were seen only in search snippets and which are community anecdote. The key sources, grouped:

## OpenTherm protocol
- OpenTherm Protocol Specification v2.2 (PDF mirror): http://ihormelnyk.com/Content/Pages/opentherm_library/Opentherm%20Protocol%20v2-2.pdf
- OpenTherm Gateway (OTGW) firmware documentation: https://otgw.tclcode.com/firmware.html
- OTGW-firmware (ESP8266 NodeMCU): https://github.com/rvdbreemen/OTGW-firmware
- Ihor Melnyk OpenTherm library: https://github.com/ihormelnyk/opentherm_library
- ESPHome OpenTherm component: https://esphome.io/components/opentherm/
- Wikipedia, OpenTherm: https://en.wikipedia.org/wiki/OpenTherm

## Immergas Victrix Tera
- Victrix Tera 24 Plus (V1) installer manual: https://www.immergas.com/media/Prodotto/63cc198a3fdcb6a15d98b7c7/Victrix-Tera-Plus-24-1046002_002_01180.pdf
- Victrix Tera V2 24 Plus installer manual (retailer copy of the Immergas document): https://gimli.freetls.fastly.net/tavolla/Products/Attachments/40503/inst.pdf
- immergas.hu, Victrix Tera 24/35 Plus (HU): https://immergas.hu/sites/default/files/termekek/kazan/dokumentumok/Victrix-Tera%2024%2035%20Plus_HU.pdf
- immergas.hu, Victrix Tera V2 28/32/38 (HU): https://immergas.hu/sites/default/files/documents/Victrix%20Tera%20V2%2028-32-38_HUN.pdf
- OTGateway (Immergas fix, issues #3 and #284): https://github.com/Laxilef/OTGateway
- HA community, Immergas Victrix integration: https://community.home-assistant.io/t/immergas-victrix-integration-with-ha/395191
- DIYLESS Thermostat 3 issues (Immergas cycling, reply loss): https://github.com/diyless/opentherm-thermostat3/issues/34, /issues/32
- jotuera Immergas OpenTherm hub notes: https://raw.githubusercontent.com/jotuera/immergas-modbus-bms-nasa-opentherm-hub/main/docs/opentherm-hub.md

## Control, condensing and cycling
- BRE for BEIS (2017), *Heating controls: compensation and TPI*: https://assets.publishing.service.gov.uk/media/5a81f66ced915d74e62350ca/heating-controls-compensation-tpi-bre.pdf
- Bennett, Elwell, Oreszczyn (UCL), boiler start/stop cycling: https://discovery.ucl.ac.uk/10054754/1/StartStop_BG_Paper_BSERT_v4_final_main_updated_figures_references.pdf
- Bosch condensing whitepaper: https://www.bosch-industrial.com/africa/media/country_pool/knowledge/technical-reports/whitepaper_condensing.pdf
- BEAMA / Salford controls testing: https://www.beama.org.uk/news/the-heat-is-on-beama-led-research-reveals-heating-controls-can-reduce-gas-use-by-10-12-percent-amid-record-price-hikes.html
- SAT (Smart Autotune Thermostat): https://github.com/Alexwijn/SAT
- Siemens RVL470 heating-curve manual: https://www.manualslib.com/manual/958756/Siemens-Rvl470.html?page=35
- Heat Geek: zoning, low-loss headers, legionella: https://heatgeek.com/articles/why-not-to-zone-heat-pumps-or-boilers, https://www.heatgeek.com/low-loss-headers/
- DOE Building America: hydronic setback, boiler cycling controls: https://www.energy.gov/sites/prod/files/2014/07/f17/ba_case_study_hydronic_setback.pdf
- HSE legionella guidance: https://hse.gov.uk/legionnaires/hot-and-cold.htm

## Learning, KPIs and safety
- Bacher & Madsen, heat dynamics of buildings (DTU): https://orbit.dtu.dk/en/publications/identifying-suitable-models-for-the-heat-dynamics-of-buildings/
- Grey-box RC identification with CasADi (arXiv 2510.21044): https://arxiv.org/html/2510.21044v1
- MPC/RL field-demonstration review (arXiv 2503.05022): https://arxiv.org/html/2503.05022v6
- Nest ErP control-class sheet: https://nest.com/support/images/misc-nest-thermostat-eu/064-00025-GB-A_A2_ErP_English.pdf
- IEC 60730-1 Annex H software classes (Microchip): https://onlinedocs.microchip.com/oxy/GUID-A619C762-0FA4-4AA9-B164-91996365FAF7-en-US-9/GUID-24711A10-AEB3-402F-8B75-B35422232E0F.html

## Hydraulics and zoning
- evohome zone heat demand (protocol wiki): https://github.com/zxdavb/The-Evohome-Protocol/wiki/3150:-Zone-Heat-Demand
- Versatile Thermostat: https://github.com/jmcollin78/versatile_thermostat
- Better Thermostat: https://github.com/KartoffelToby/better_thermostat
- Danfoss on balancing (HVP): https://www.hvpmag.co.uk/Danfoss-outlines-the-benefits-of-heating-system-balancing/13075

## Known gaps
- No public OpenTherm log from a Tera: the supported IDs, fault mapping and OT version are unknown until tested.
- The meaning of Immergas's 0xCA byte and TSP 12 is undocumented.
- The time to E31 and recovery without a power cycle are undocumented.
- The OpenTherm v3/v4 specs are members-only; v4 details come from implementations.
- Official tado, Netatmo and Honeywell compatibility lists, and Hungarian forum experience, could not be retrieved.
