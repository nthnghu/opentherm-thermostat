#pragma once
// Simulated heating plant: a 1R1C house, the radiator water loop, a Victrix-Tera-like boiler and a DHW tank.
// Pure C (no ESP-IDF). The physics follow docs/research/interactive/room-sim.html; the boiler numbers follow
// docs/research/03-victrix-tera-24-plus.md (4.3 kW minimum, cut-out at target + 5 K, t4 anti-cycle, t5 ramp,
// DHW priority at tank setpoint + A4).
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    // House
    float loss_kw;      // design heat loss
    float t_design;     // design outdoor temperature (°C) for loss_kw at 21 °C inside
    float tau_h;        // building time constant (h)
    float r50_kw;       // radiator output at ΔT 50 K
    float water_l;      // system water volume (L)
    float t_out;        // mean outdoor temperature (°C)
    float out_swing;    // ± daily swing (K), warmest at 15:00
    // Boiler
    float pmin_kw, pmax_kw, pmax_dhw_kw;
    float t4_s;         // anti-cycle wait after a CH cut-out
    float cut_k;        // flame off when flow > target + cut_k
    float t0_c;         // lowest flow target the boiler accepts
    float t5_s;         // ramp from min to max power after ignition
    float pressure_bar;
    float leak_bar_per_day;
    // DHW tank
    float tank_l;
    float tank_set_c;   // panel DHW setpoint
    float a4_k;         // tank charging flow = tank_set + a4
    float a5_k;         // charge when tank < tank_set - a5
    bool dhw_draws;     // morning and evening draws
} plant_params_t;

typedef struct {
    bool ch_enable;     // CH demand as the boiler sees it (after OT quirks)
    float target_c;     // flow target the boiler heats to
    bool dhw_enable;
} plant_inputs_t;

typedef struct {
    double t_s;         // simulated time since start
    float tod_start_h;  // time of day at t = 0
    float room_c, water_c, tank_c, out_c;
    float flow_c, return_c;
    float power_kw;
    float modulation;   // % of the CH range (above 100 during DHW)
    float pressure_bar;
    bool flame, dhw_active, ch_active;
    double ac_until_s, ign_s;
    uint32_t starts, ch_starts, dhw_starts;
    double burner_s, dhw_burner_s;
    double heat_kwh, gas_kwh;
} plant_state_t;

void plant_default_params(plant_params_t *p);
void plant_init(plant_state_t *s, const plant_params_t *p, float room_c, float tod_start_h);
// Advances the model by dt seconds (sub-steps internally at <= 2 s).
void plant_step(plant_state_t *s, const plant_params_t *p, const plant_inputs_t *in, float dt);
// Illustrative NCV efficiency (%) for a return temperature (same curve as condensing.html).
float plant_eta_ncv(float return_c);
// Flow temperature at which the minimum burner output is in balance with the radiators (the "OPV").
float plant_min_fire_flow(const plant_state_t *s, const plant_params_t *p);
