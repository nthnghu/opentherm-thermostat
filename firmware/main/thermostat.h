#pragma once
// Thermostat core: runs in its own high-priority task on core 1, independent of the UI.
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "control.h"
#include "schedule.h"

#define THERMO_SP_MIN   5.0f
#define THERMO_SP_MAX   30.0f
#define THERMO_SENSOR_STALE_S 30  // no reading for this long -> sensor lost (behaviour: ctrl loss_mode)

typedef enum { THERMO_MODE_OFF = 0, THERMO_MODE_HEAT = 1 } thermo_mode_t;

typedef struct {
    // Sensor
    bool sensor_ok;         // a fresh reading within THERMO_SENSOR_STALE_S
    int sensor_age_s;       // seconds since the last good reading (-1 = none yet)
    float room_c;           // smoothed (~30 s average), offset applied; used for control
    float humidity;         // smoothed
    float room_raw;         // last single reading, offset applied
    float room_shown;       // what the screens show: 0.1 °C steps with a dead band, so it doesn't flap
    float humidity_shown;   // whole percent with a dead band
    uint32_t sensor_errors;
    // Control
    float setpoint;         // the user's setpoint = Comfort
    float target_c;         // what the control law works towards (schedule, boost, away; schedule.h)
    thermo_mode_t mode;
    bool demand;            // heat requested from the boiler
    float flow_setpoint;    // °C requested (OT ID 1)
    ctrl_state_t ctrl_state;
    float req_c;            // flow the room asks for (before the low-load manager)
    float pi_p, pi_i;
    float duty;
    uint16_t starts_last_h; // CH burner starts in the last hour
    // Boiler (OpenTherm)
    bool boiler_sim;
    bool sim_room;          // bench: the simulated house replaces the SHT40 temperature
    bool boiler_online;     // answered recently
    bool flame;
    bool dhw_active;
    float flow_c;
    float return_c;
    float modulation;
    uint32_t ot_ok, ot_errors;
    // Schedule and hot water (schedule.c)
    uint8_t sched_src;      // sched_src_t
    int next_change_min;    // -1 = none
    uint8_t next_level;     // sched_level_t
    int boost_left_s;
    bool away;
    int64_t away_until;     // 0 = until cancelled
    bool summer;
    bool outside_valid, outside_mean_valid;
    float outside_c, outside_mean_c;
    bool dhw_enable;
    uint8_t dhw_reason;     // sched_dhw_reason_t
    int dhw_boost_left_s;
    float dhw_set;          // ID 56 value being written (0 = panel value)
    uint8_t id56;           // otb_dhwset_t
    bool tank_valid;
    float tank_c;
    bool pasteur_running;
    uint8_t pasteur_result; // sched_pasteur_result_t
    int64_t pasteur_last;
    // Settings
    float offset;           // °C added to the sensor reading
    float hysteresis;       // °C either side of the setpoint (on/off strategy; = ctrl onoff_hyst_k)
    uint32_t loops;         // control loop counter (to check the cadence)
} thermo_state_t;

typedef struct {
    time_t t;               // 0 if the clock was not set at the time of the sample
    int16_t room_c10;       // °C x10, INT16_MIN = no reading
    uint8_t humidity;
    int16_t setpoint_c10;
    uint8_t demand;
} thermo_sample_t;

#define THERMO_HISTORY_LEN 1440  // 24 h at 1 / min

void thermo_start(void);
void thermo_get(thermo_state_t *out);

void thermo_set_setpoint(float c);
void thermo_adjust_setpoint(float delta);
void thermo_set_mode(thermo_mode_t m);
void thermo_set_offset(float c);
void thermo_set_hysteresis(float c);
void thermo_set_boiler_sim(bool sim);
void thermo_set_sim_room(bool on);

// Control settings (control.h); saved to NVS a few seconds after a change.
void thermo_get_ctrl_config(ctrl_config_t *out);
void thermo_set_ctrl_config(const ctrl_config_t *c);

// Schedule settings (schedule.h); saved like the control settings.
void thermo_get_sched_config(sched_config_t *out);
void thermo_set_sched_config(const sched_config_t *c);
// User actions; false when the feature is switched off.
bool thermo_boost(bool on);
bool thermo_dhw_boost(bool on);
bool thermo_away(bool on, int64_t until);  // until = epoch, 0 = until cancelled (saved across restarts)
bool thermo_pasteurise(bool on);

// Copies up to max samples, oldest first; returns the count.
int thermo_history(thermo_sample_t *out, int max);
