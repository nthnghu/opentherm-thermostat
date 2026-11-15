#include "plant_sim.h"
#include <math.h>

#define CP_WATER 4.186f       // kJ/(kg·K)
#define PUMP_KG_S 0.25f       // fixed pump flow
#define MCP (PUMP_KG_S * CP_WATER)
#define RAD_N 1.3f
#define T_COLD 12.0f          // mains water

void plant_default_params(plant_params_t *p)
{
    *p = (plant_params_t){
        .loss_kw = 10, .t_design = -13, .tau_h = 40, .r50_kw = 16, .water_l = 80, .t_out = 5, .out_swing = 4,
        .pmin_kw = 4.3f, .pmax_kw = 24, .pmax_dhw_kw = 28, .t4_s = 180, .cut_k = 5, .t0_c = 25, .t5_s = 180,
        .pressure_bar = 1.5f, .leak_bar_per_day = 0,
        .tank_l = 120, .tank_set_c = 50, .a4_k = 25, .a5_k = 3, .dhw_draws = true,
    };
}

void plant_init(plant_state_t *s, const plant_params_t *p, float room_c, float tod_start_h)
{
    *s = (plant_state_t){0};
    s->tod_start_h = tod_start_h;
    s->room_c = room_c;
    s->water_c = room_c + 5;
    s->tank_c = p->tank_set_c;
    s->out_c = p->t_out;
    s->flow_c = s->return_c = s->water_c;
    s->pressure_bar = p->pressure_bar;
    s->ac_until_s = 0;
}

float plant_eta_ncv(float ret)
{
    float sens = 97.2f + 0.03f * (80 - fminf(80, ret));
    float gain = ret < 57 ? 10.5f * (1 - expf(-(57 - ret) / 20)) : 0;
    return fminf(110.5f, sens + gain);
}

static float ua(const plant_params_t *p)
{
    return p->loss_kw / (21.0f - p->t_design);
}

static float q_rad(const plant_params_t *p, float water, float room)
{
    return water > room ? p->r50_kw * powf((water - room) / 50.0f, RAD_N) : 0;
}

float plant_min_fire_flow(const plant_state_t *s, const plant_params_t *p)
{
    float mean = s->room_c + 50.0f * powf(p->pmin_kw / p->r50_kw, 1.0f / RAD_N);
    return mean + p->pmin_kw / (2 * MCP);
}

static float tod_h(const plant_state_t *s)
{
    return fmodf(s->tod_start_h + (float)(s->t_s / 3600.0), 24.0f);
}

// Litres drawn per second at this time of day: 40 L around 07:00, 60 L around 19:30 (10 min each).
static float draw_lps(float h)
{
    if (h >= 7.0f && h < 7.0f + 10 / 60.0f) {
        return 40.0f / 600;
    }
    if (h >= 19.5f && h < 19.5f + 10 / 60.0f) {
        return 60.0f / 600;
    }
    return 0;
}

static void step(plant_state_t *s, const plant_params_t *p, const plant_inputs_t *in, float dt)
{
    const float UA = ua(p);
    const float c_house = p->tau_h * 3600 * UA;    // kJ/K
    const float c_water = p->water_l * CP_WATER + 25; // kJ/K incl. boiler metal
    const float c_tank = p->tank_l * CP_WATER;
    float h = tod_h(s);
    s->out_c = p->t_out + p->out_swing * cosf((h - 15) / 24 * 2 * (float)M_PI);

    // DHW draws and tank losses
    if (p->dhw_draws) {
        float l = draw_lps(h) * dt;
        s->tank_c -= l * (s->tank_c - T_COLD) / p->tank_l;
    }
    s->tank_c -= dt * (s->tank_c - 20) / (36 * 3600.0f);  // standing loss

    // DHW priority (tank NTC on the boiler)
    if (!s->dhw_active && in->dhw_enable && s->tank_c < p->tank_set_c - p->a5_k) {
        s->dhw_active = true;  // a running CH burn simply continues as a DHW burn (no new start)
    }
    if (s->dhw_active && (!in->dhw_enable || s->tank_c >= p->tank_set_c)) {
        s->dhw_active = false;
        s->flame = false;
        s->power_kw = 0;
        s->ac_until_s = s->t_s;  // no anti-cycle wait after DHW
    }

    float flow_now = s->water_c + (s->flame && !s->dhw_active ? s->power_kw / (2 * MCP) : 0);
    if (s->dhw_active) {
        if (!s->flame) {
            s->flame = true;
            s->ign_s = s->t_s;
            s->starts++;
            s->dhw_starts++;
            s->power_kw = p->pmin_kw;
        }
        float cap = p->pmin_kw + (p->pmax_dhw_kw - p->pmin_kw) * fminf(1, (float)(s->t_s - s->ign_s) / p->t5_s);
        s->power_kw = cap;
        s->tank_c += s->power_kw * dt / c_tank;
        s->dhw_burner_s += dt;
        s->burner_s += dt;
        s->flow_c = fminf(p->tank_set_c + p->a4_k, s->tank_c + 15);
        s->return_c = s->tank_c + 5;
        s->ch_active = false;
    } else {
        float target = in->ch_enable && in->target_c > 10.5f ? fmaxf(in->target_c, p->t0_c) : 0;
        if (s->flame) {
            if (!target || flow_now > target + p->cut_k) {
                s->flame = false;
                s->power_kw = 0;
                s->ac_until_s = s->t_s + p->t4_s;
            } else {
                float cap = p->pmin_kw + (p->pmax_kw - p->pmin_kw) * fminf(1, (float)(s->t_s - s->ign_s) / p->t5_s);
                float pw = s->power_kw + 0.04f * (target - flow_now) * dt;
                s->power_kw = fmaxf(p->pmin_kw, fminf(cap, pw));
            }
        } else if (target && s->t_s >= s->ac_until_s && flow_now < target) {
            s->flame = true;
            s->ign_s = s->t_s;
            s->power_kw = p->pmin_kw;
            s->starts++;
            s->ch_starts++;
        }
        s->ch_active = s->flame;
        if (s->flame) {
            s->burner_s += dt;
        }
    }

    // Radiator loop and house
    float q = q_rad(p, s->water_c, s->room_c);
    float ch_power = s->flame && !s->dhw_active ? s->power_kw : 0;
    if (!s->dhw_active) {
        s->flow_c = s->water_c + ch_power / (2 * MCP);
        s->return_c = s->water_c - q / (2 * MCP);
    }
    s->water_c += (ch_power - q) * dt / c_water;
    s->room_c += (q - UA * (s->room_c - s->out_c)) * dt / c_house;

    // Energy and modulation
    if (s->flame) {
        float eta = plant_eta_ncv(s->return_c) / 100;
        s->heat_kwh += s->power_kw * dt / 3600;
        s->gas_kwh += s->power_kw / eta * dt / 3600;
        s->modulation = (s->power_kw - p->pmin_kw) / (p->pmax_kw - p->pmin_kw) * 100;
    } else {
        s->modulation = 0;
    }
    s->pressure_bar -= p->leak_bar_per_day * dt / 86400;
    s->t_s += dt;
}

void plant_step(plant_state_t *s, const plant_params_t *p, const plant_inputs_t *in, float dt)
{
    while (dt > 0) {
        float d = dt > 2 ? 2 : dt;
        step(s, p, in, d);
        dt -= d;
    }
}
