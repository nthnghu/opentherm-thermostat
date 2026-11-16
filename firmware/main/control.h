#pragma once
// Heating control law (docs/research/10-recommendation-phase1.md §4–5). Pure C (no ESP-IDF), host-tested.
//
// Strategy 0, on/off baseline: hysteresis around the setpoint, fixed flow temperature.
// Strategy 1, PI load compensation: flow = base + Kp·e + I (updated every 60 s), clamped, rate-limited;
//   the integral freezes during DHW, duty-off, saturation and while the boiler is unreachable.
// Low-load manager (strategy 1): when the requested flow is below the minimum-fire flow it switches to planned
//   duty cycles (long burns, max starts per hour); during a burn at minimum fire it holds TSet = flow − hold_k.
// Safety: sensor-loss behaviour (off / fixed flow with a duty limit), frost floor, UFH floor.
// Every feature has its own switch in ctrl_config_t.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CTRL_CONFIG_VERSION 1

typedef enum { CTRL_STRATEGY_ONOFF = 0, CTRL_STRATEGY_PI = 1 } ctrl_strategy_t;
typedef enum {
    CTRL_LOSS_DISABLED = 0,  // feature off: on sensor loss the controller keeps its last decision (not advised)
    CTRL_LOSS_HEAT_OFF = 1,  // heating off (the 0.1 behaviour; on this boiler the house goes cold)
    CTRL_LOSS_FIXED = 2,     // fixed conservative flow with a duty limit (recommended)
} ctrl_loss_t;

typedef struct {
    uint16_t version;
    uint8_t strategy;        // ctrl_strategy_t
    // Limits
    float flow_max_c;        // never ask for more
    float flow_min_c;        // minimum-fire flow ("OPV"); measure on the real boiler (test 12)
    // On/off baseline
    float onoff_hyst_k;
    float onoff_flow_c;
    // PI
    float pi_base_c;
    float pi_kp;             // °C flow per K room error
    float pi_ti_h;           // integral time
    float pi_deadband_k;     // no integration inside ± this
    float pi_i_limit_k;      // |I| limit
    float rate_k_per_min;    // TSet slew limit (0 = off)
    // Low load
    bool hold_on;            // minimum-fire hold
    float hold_k;            // TSet = flow − hold_k during a burn at minimum fire
    bool duty_on;            // duty cycling below the minimum-fire flow
    uint16_t duty_min_burn_s;
    uint16_t duty_min_off_s;
    uint8_t max_starts_h;
    // Integral freeze while the boiler heats the tank
    bool freeze_on_dhw;
    // Sensor loss
    uint8_t loss_mode;       // ctrl_loss_t
    float loss_flow_c;
    uint8_t loss_duty_pct;   // % of each 30 min period
    // Frost floor
    bool frost_on;
    float frost_room_c;      // heat when the room falls below this (any mode)
    float frost_flow_c;
    // Underfloor floor (thermostatic mixer needs flow above its setting)
    bool ufh_on;
    float ufh_mixer_c;
    float ufh_margin_k;
} ctrl_config_t;

typedef enum {
    CTRL_ST_OFF = 0,     // no demand
    CTRL_ST_ONOFF,       // baseline burner demand
    CTRL_ST_MOD,         // modulating on the PI flow
    CTRL_ST_HOLD,        // modulating, minimum-fire hold active
    CTRL_ST_DUTY_ON,
    CTRL_ST_DUTY_OFF,
    CTRL_ST_SENSOR_LOSS, // fallback
    CTRL_ST_FROST,
} ctrl_state_t;

typedef struct {
    double now_s;
    bool heat_mode;      // mode Heat (false = Off: only the frost floor heats)
    float target_c;      // room setpoint
    bool room_ok;
    float room_c;
    bool boiler_ok;      // OpenTherm link up
    bool flame, dhw_active;
    bool flow_valid;
    float flow_c;
    bool mod_valid;
    float modulation;    // %
} ctrl_in_t;

typedef struct {
    bool ch_enable;
    float tset;
    ctrl_state_t state;
    float req_c;         // flow the room asks for (before the low-load manager)
    float p, i;          // PI terms
    float duty;          // 0..1 while duty cycling
    uint16_t starts_last_h;  // CH burner starts in the last hour (DHW excluded)
} ctrl_out_t;

typedef struct {
    ctrl_config_t cfg;
    // PI
    double last_pi_s;
    float i, p, u, u_limited;
    bool pi_started;
    // Low load
    int phase;           // ctrl_state_t of the low-load manager
    double phase_until_s, burn_end_s;
    double flame_since_s;
    bool last_flame;
    // On/off and fallback
    bool onoff_on;
    double loss_period_start_s;
    // Start history (CH only), ring of start times
    double starts[16];
    int start_head;
    float last_tset;
    double last_s;
    ctrl_out_t out;
} ctrl_t;

void ctrl_default_config(ctrl_config_t *c);
// Clamps every field to its allowed range (after loading or editing).
void ctrl_sanitize(ctrl_config_t *c);
void ctrl_init(ctrl_t *c, const ctrl_config_t *cfg);
void ctrl_set_config(ctrl_t *c, const ctrl_config_t *cfg);  // keeps the controller state
// Sets the PI long-term correction (after a restart, from the last saved minute), within the integral limit.
void ctrl_set_integral(ctrl_t *c, float i);
// Call once per second.
const ctrl_out_t *ctrl_step(ctrl_t *c, const ctrl_in_t *in);
const char *ctrl_state_name(ctrl_state_t s);

// Named access to the configuration (serial CFG command, web, JSON export).
int ctrl_config_count(void);
const char *ctrl_config_name(int idx);
const char *ctrl_config_help(int idx);
// Field description (type, range) for generic editors; NULL past the end.
const struct ct_field *ctrl_config_field(int idx);
// Formats field idx as text. Returns false for a bad index.
bool ctrl_config_get(const ctrl_config_t *c, int idx, char *out, size_t size);
// Parses and range-checks a value. Returns false (and leaves c unchanged) on a bad name or value.
bool ctrl_config_set(ctrl_config_t *c, const char *name, const char *value);
