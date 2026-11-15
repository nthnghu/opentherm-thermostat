#include "control.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PI_PERIOD_S     60.0
#define HOLD_AFTER_S    60.0    // a burn must have settled before the minimum-fire hold applies
#define HOLD_MOD_PCT    5.0f    // "at minimum fire" when the reported modulation is at or below this
#define DUTY_NOMINAL_S  1200.0  // on-time scale for the duty fraction
#define DUTY_MIN_FRAC   0.08f   // below this: stay off for a while instead of a token burn
#define DUTY_IDLE_S     600.0
#define LOSS_PERIOD_S   1800.0
#define FROST_HYST_K    0.5f
#define BOILER_T0       25.0f   // lowest flow target a boiler accepts

/* ------------------------------------------------------------- config */

typedef enum { T_BOOL, T_U8, T_U16, T_FLOAT } ftype_t;

typedef struct {
    const char *name;
    ftype_t type;
    size_t off;
    float min, max;
    const char *help;
} field_t;

#define F(n, t, lo, hi, h) {#n, t, offsetof(ctrl_config_t, n), lo, hi, h}
static const field_t FIELDS[] = {
    F(strategy, T_U8, 0, 1, "0 = on/off baseline (fixed flow), 1 = PI load compensation"),
    F(flow_max_c, T_FLOAT, 30, 80, "highest flow setpoint ever sent (°C)"),
    F(flow_min_c, T_FLOAT, 25, 60, "minimum-fire flow (°C); below it the low-load manager takes over"),
    F(onoff_hyst_k, T_FLOAT, 0.05f, 1, "on/off: hysteresis either side of the setpoint (K)"),
    F(onoff_flow_c, T_FLOAT, 30, 80, "on/off: fixed flow setpoint (°C)"),
    F(pi_base_c, T_FLOAT, 20, 60, "PI: base flow (°C)"),
    F(pi_kp, T_FLOAT, 1, 40, "PI: gain (°C flow per K room error)"),
    F(pi_ti_h, T_FLOAT, 0.25f, 12, "PI: integral time (h)"),
    F(pi_deadband_k, T_FLOAT, 0, 0.5f, "PI: no integration within ± this error (K)"),
    F(pi_i_limit_k, T_FLOAT, 0, 30, "PI: integral limit (K of flow)"),
    F(rate_k_per_min, T_FLOAT, 0, 10, "flow setpoint slew limit (K/min, 0 = off)"),
    F(hold_on, T_BOOL, 0, 1, "minimum-fire hold on/off"),
    F(hold_k, T_FLOAT, 0, 10, "minimum-fire hold: setpoint = flow − this (K)"),
    F(duty_on, T_BOOL, 0, 1, "duty cycling below the minimum-fire flow on/off"),
    F(duty_min_burn_s, T_U16, 180, 1800, "duty: shortest planned burn (s)"),
    F(duty_min_off_s, T_U16, 60, 3600, "duty: shortest pause (s)"),
    F(max_starts_h, T_U8, 1, 12, "duty: most CH burner starts per hour"),
    F(freeze_on_dhw, T_BOOL, 0, 1, "freeze the PI integral while the boiler heats the tank"),
    F(loss_mode, T_U8, 0, 2, "room sensor lost: 0 = feature off, 1 = heating off, 2 = fixed flow with duty"),
    F(loss_flow_c, T_FLOAT, 25, 70, "sensor lost: fixed flow (°C)"),
    F(loss_duty_pct, T_U8, 0, 100, "sensor lost: % of each 30 min period with heat"),
    F(frost_on, T_BOOL, 0, 1, "frost floor on/off (heats in any mode below frost_room_c)"),
    F(frost_room_c, T_FLOAT, 2, 12, "frost floor: room temperature (°C)"),
    F(frost_flow_c, T_FLOAT, 25, 60, "frost floor: flow setpoint (°C)"),
    F(ufh_on, T_BOOL, 0, 1, "underfloor floor on/off (flow ≥ mixer + margin while heating)"),
    F(ufh_mixer_c, T_FLOAT, 20, 55, "underfloor: thermostatic mixer setting (°C)"),
    F(ufh_margin_k, T_FLOAT, 0, 15, "underfloor: margin above the mixer setting (K)"),
};
#define NFIELDS ((int)(sizeof(FIELDS) / sizeof(FIELDS[0])))

void ctrl_default_config(ctrl_config_t *c)
{
    *c = (ctrl_config_t){
        .version = CTRL_CONFIG_VERSION,
        .strategy = CTRL_STRATEGY_PI,
        .flow_max_c = 55, .flow_min_c = 40,
        .onoff_hyst_k = 0.2f, .onoff_flow_c = 50,
        .pi_base_c = 35, .pi_kp = 12, .pi_ti_h = 2.5f, .pi_deadband_k = 0.1f, .pi_i_limit_k = 20, .rate_k_per_min = 1,
        .hold_on = true, .hold_k = 3,
        .duty_on = true, .duty_min_burn_s = 600, .duty_min_off_s = 240, .max_starts_h = 3,
        .freeze_on_dhw = true,
        .loss_mode = CTRL_LOSS_FIXED, .loss_flow_c = 42, .loss_duty_pct = 40,
        .frost_on = true, .frost_room_c = 6, .frost_flow_c = 35,
        .ufh_on = false, .ufh_mixer_c = 35, .ufh_margin_k = 5,
    };
}

static float get_f(const ctrl_config_t *c, const field_t *f)
{
    const uint8_t *p = (const uint8_t *)c + f->off;
    switch (f->type) {
    case T_BOOL: return *(const bool *)p;
    case T_U8: return *p;
    case T_U16: return *(const uint16_t *)p;
    default: return *(const float *)p;
    }
}

static void put_f(ctrl_config_t *c, const field_t *f, float v)
{
    uint8_t *p = (uint8_t *)c + f->off;
    if (isnan(v)) {
        v = f->min;
    }
    v = fminf(fmaxf(v, f->min), f->max);
    switch (f->type) {
    case T_BOOL: *(bool *)p = v != 0; break;
    case T_U8: *p = (uint8_t)lroundf(v); break;
    case T_U16: *(uint16_t *)p = (uint16_t)lroundf(v); break;
    default: *(float *)p = v; break;
    }
}

void ctrl_sanitize(ctrl_config_t *c)
{
    for (int i = 0; i < NFIELDS; i++) {
        put_f(c, &FIELDS[i], get_f(c, &FIELDS[i]));
    }
    if (c->flow_min_c > c->flow_max_c) {
        c->flow_min_c = c->flow_max_c;
    }
    c->version = CTRL_CONFIG_VERSION;
}

int ctrl_config_count(void)
{
    return NFIELDS;
}

const char *ctrl_config_name(int idx)
{
    return idx >= 0 && idx < NFIELDS ? FIELDS[idx].name : NULL;
}

const char *ctrl_config_help(int idx)
{
    return idx >= 0 && idx < NFIELDS ? FIELDS[idx].help : NULL;
}

bool ctrl_config_get(const ctrl_config_t *c, int idx, char *out, size_t size)
{
    if (idx < 0 || idx >= NFIELDS) {
        return false;
    }
    const field_t *f = &FIELDS[idx];
    float v = get_f(c, f);
    if (f->type == T_FLOAT) {
        snprintf(out, size, "%.2f", v);
    } else {
        snprintf(out, size, "%d", (int)v);
    }
    return true;
}

bool ctrl_config_set(ctrl_config_t *c, const char *name, const char *value)
{
    for (int i = 0; i < NFIELDS; i++) {
        const field_t *f = &FIELDS[i];
        if (strcmp(f->name, name) != 0) {
            continue;
        }
        float v;
        if (f->type == T_BOOL && (strcasecmp(value, "on") == 0 || strcasecmp(value, "true") == 0)) {
            v = 1;
        } else if (f->type == T_BOOL && (strcasecmp(value, "off") == 0 || strcasecmp(value, "false") == 0)) {
            v = 0;
        } else {
            char *end;
            v = strtof(value, &end);
            if (end == value || *end) {
                return false;
            }
        }
        if (v < f->min || v > f->max) {
            return false;
        }
        put_f(c, f, v);
        if (c->flow_min_c > c->flow_max_c) {
            c->flow_min_c = c->flow_max_c;
        }
        return true;
    }
    return false;
}

/* ---------------------------------------------------------- controller */

const char *ctrl_state_name(ctrl_state_t s)
{
    static const char *const n[] = {"off", "on/off", "modulating", "min-fire hold", "duty on", "duty off",
                                    "sensor lost", "frost"};
    return s <= CTRL_ST_FROST ? n[s] : "?";
}

void ctrl_init(ctrl_t *c, const ctrl_config_t *cfg)
{
    memset(c, 0, sizeof(*c));
    c->cfg = *cfg;
    ctrl_sanitize(&c->cfg);
    c->phase = CTRL_ST_OFF;
    c->last_s = -1;
    for (int i = 0; i < 16; i++) {
        c->starts[i] = -1e9;
    }
}

void ctrl_set_config(ctrl_t *c, const ctrl_config_t *cfg)
{
    c->cfg = *cfg;
    ctrl_sanitize(&c->cfg);
    if (fabsf(c->i) > c->cfg.pi_i_limit_k) {
        c->i = copysignf(c->cfg.pi_i_limit_k, c->i);
    }
}

static int starts_last_hour(const ctrl_t *c, double now, double *oldest)
{
    int n = 0;
    double o = now;
    for (int i = 0; i < 16; i++) {
        if (now - c->starts[i] < 3600) {
            n++;
            if (c->starts[i] < o) {
                o = c->starts[i];
            }
        }
    }
    if (oldest) {
        *oldest = o;
    }
    return n;
}

static float clampf(float v, float lo, float hi)
{
    return fminf(fmaxf(v, lo), hi);
}

// PI update (every 60 s); returns the requested flow.
static void pi_update(ctrl_t *c, const ctrl_in_t *in)
{
    const ctrl_config_t *k = &c->cfg;
    float e = in->target_c - in->room_c;
    bool freeze = (k->freeze_on_dhw && in->dhw_active) || c->phase == CTRL_ST_DUTY_OFF || !in->boiler_ok;
    float p = k->pi_kp * e;
    float u0 = k->pi_base_c + p + c->i;
    bool sat = (u0 >= k->flow_max_c && e > 0) || (u0 <= 20 && e < 0);
    if (!freeze && !sat && fabsf(e) > k->pi_deadband_k && k->pi_ti_h > 0) {
        c->i += k->pi_kp / (k->pi_ti_h * 3600.0f) * e * (float)PI_PERIOD_S;
        c->i = clampf(c->i, -k->pi_i_limit_k, k->pi_i_limit_k);
    }
    c->p = p;
    c->u = clampf(k->pi_base_c + p + c->i, 10, k->flow_max_c);
}

static void set_out(ctrl_t *c, bool ch, float tset, ctrl_state_t st)
{
    c->out.ch_enable = ch;
    c->out.tset = ch ? clampf(tset, 10, c->cfg.flow_max_c) : 10;
    c->out.state = st;
}

// Low-load manager for a requested flow r.
static void low_load(ctrl_t *c, const ctrl_in_t *in, float r)
{
    const ctrl_config_t *k = &c->cfg;
    const double now = in->now_s;
    const float o = k->flow_min_c;
    bool modulating = c->phase == CTRL_ST_MOD || c->phase == CTRL_ST_HOLD;
    bool settled = in->flame && !in->dhw_active && now - c->flame_since_s > HOLD_AFTER_S && in->flow_valid;

    if (r >= o + 1 || (modulating && r >= o - 1) || (!k->duty_on && r >= BOILER_T0)) {
        float t = r;
        ctrl_state_t st = CTRL_ST_MOD;
        if (k->hold_on && settled && in->mod_valid && in->modulation <= HOLD_MOD_PCT && in->flow_c - k->hold_k > t) {
            t = in->flow_c - k->hold_k;
            st = CTRL_ST_HOLD;
        }
        c->phase = st;
        c->out.duty = 1;
        set_out(c, true, t, st);
        return;
    }
    if (!k->duty_on) {  // too little demand for even the lowest boiler target
        c->phase = CTRL_ST_OFF;
        c->out.duty = 0;
        set_out(c, false, 10, CTRL_ST_OFF);
        return;
    }

    // Duty cycling
    float d = o > in->room_c ? clampf(powf(fmaxf(0, (r - in->room_c) / (o - in->room_c)), 1.3f), 0, 1) : 1;
    c->out.duty = d;
    if (modulating || c->phase == CTRL_ST_OFF) {
        c->phase = CTRL_ST_DUTY_OFF;
        c->phase_until_s = now;  // decide now
        c->burn_end_s = now;
    }
    if (now >= c->phase_until_s) {
        double oldest;
        int starts = starts_last_hour(c, now, &oldest);
        if (d < DUTY_MIN_FRAC) {
            c->phase = CTRL_ST_DUTY_OFF;
            c->phase_until_s = now + DUTY_IDLE_S;
        } else if (starts >= k->max_starts_h) {
            c->phase = CTRL_ST_DUTY_OFF;  // wait until the oldest start leaves the hour
            c->phase_until_s = oldest + 3600 + 1;
        } else {
            double on = fmax(k->duty_min_burn_s, d * DUTY_NOMINAL_S);
            double cycle = fmin(3600, fmax(on / d, 3600.0 / k->max_starts_h));
            double off = fmax(cycle - on, k->duty_min_off_s);
            c->phase = CTRL_ST_DUTY_ON;
            c->burn_end_s = now + on;
            c->phase_until_s = now + on + off;
        }
    }
    if (c->phase == CTRL_ST_DUTY_ON && now < c->burn_end_s) {
        float t = o;
        if (k->hold_on && settled) {
            t = clampf(in->flow_c - k->hold_k, BOILER_T0, k->flow_max_c);
        }
        set_out(c, true, t, CTRL_ST_DUTY_ON);
    } else {
        if (c->phase == CTRL_ST_DUTY_ON) {
            c->phase = CTRL_ST_DUTY_OFF;
        }
        set_out(c, false, 10, CTRL_ST_DUTY_OFF);
    }
}

const ctrl_out_t *ctrl_step(ctrl_t *c, const ctrl_in_t *in)
{
    const ctrl_config_t *k = &c->cfg;
    const double now = in->now_s;
    double dt = c->last_s < 0 ? 1 : now - c->last_s;
    c->last_s = now;

    // CH burner starts (DHW burns excluded)
    if (in->flame && !c->last_flame) {
        c->flame_since_s = now;
        if (!in->dhw_active) {
            c->starts[c->start_head] = now;
            c->start_head = (c->start_head + 1) % 16;
        }
    }
    c->last_flame = in->flame;
    c->out.starts_last_h = (uint16_t)starts_last_hour(c, now, NULL);

    // Frost floor, latched until the room is FROST_HYST_K above the threshold
    bool frost = false;
    if (k->frost_on && in->room_ok) {
        frost = in->room_c < k->frost_room_c ||
                (c->out.state == CTRL_ST_FROST && in->room_c < k->frost_room_c + FROST_HYST_K);
    }

    if (!in->room_ok) {
        c->pi_started = false;  // restart the PI period when the sensor returns; the integral is kept
        switch (k->loss_mode) {
        case CTRL_LOSS_DISABLED:
            return &c->out;  // keep the last decision
        case CTRL_LOSS_FIXED:
            if (in->heat_mode) {
                if (c->out.state != CTRL_ST_SENSOR_LOSS) {
                    c->loss_period_start_s = now;
                }
                double pos = fmod(now - c->loss_period_start_s, LOSS_PERIOD_S);
                bool on = pos < LOSS_PERIOD_S * k->loss_duty_pct / 100.0;
                set_out(c, on, k->loss_flow_c, CTRL_ST_SENSOR_LOSS);
                c->phase = CTRL_ST_OFF;
                return &c->out;
            }
            set_out(c, false, 10, CTRL_ST_OFF);
            return &c->out;
        default:
            set_out(c, false, 10, CTRL_ST_OFF);
            c->phase = CTRL_ST_OFF;
            return &c->out;
        }
    }

    if (!in->heat_mode) {
        c->phase = CTRL_ST_OFF;
        c->onoff_on = false;
        if (frost) {
            set_out(c, true, k->frost_flow_c, CTRL_ST_FROST);
        } else {
            set_out(c, false, 10, CTRL_ST_OFF);
        }
        c->out.req_c = 0;
        c->out.p = 0;
        c->out.duty = 0;
        return &c->out;
    }

    if (k->strategy == CTRL_STRATEGY_ONOFF) {
        if (in->room_c < in->target_c - k->onoff_hyst_k) {
            c->onoff_on = true;
        } else if (in->room_c > in->target_c + k->onoff_hyst_k) {
            c->onoff_on = false;
        }
        c->phase = CTRL_ST_OFF;
        if (c->onoff_on || frost) {
            set_out(c, true, fmaxf(k->onoff_flow_c, frost ? k->frost_flow_c : 0), frost ? CTRL_ST_FROST : CTRL_ST_ONOFF);
        } else {
            set_out(c, false, 10, CTRL_ST_OFF);
        }
        c->out.req_c = c->out.tset;
    } else {
        if (!c->pi_started || now - c->last_pi_s >= PI_PERIOD_S) {
            if (!c->pi_started) {
                c->u_limited = c->u > 0 ? c->u : k->pi_base_c;
            }
            c->pi_started = true;
            c->last_pi_s = now;
            pi_update(c, in);
        }
        // Slew limit, applied every second
        if (k->rate_k_per_min > 0) {
            float step = k->rate_k_per_min * (float)dt / 60.0f;
            c->u_limited += clampf(c->u - c->u_limited, -step, step);
        } else {
            c->u_limited = c->u;
        }
        float r = c->u_limited;
        if (frost) {
            r = fmaxf(r, k->frost_flow_c);
        }
        c->out.req_c = r;
        c->out.p = c->p;
        c->out.i = c->i;
        low_load(c, in, r);
        if (frost && c->out.ch_enable) {
            c->out.state = CTRL_ST_FROST;
        }
    }

    // Underfloor floor
    if (k->ufh_on && c->out.ch_enable) {
        c->out.tset = clampf(fmaxf(c->out.tset, k->ufh_mixer_c + k->ufh_margin_k), 10, k->flow_max_c);
    }
    return &c->out;
}
