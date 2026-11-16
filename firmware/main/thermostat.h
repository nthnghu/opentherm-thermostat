#pragma once
// Thermostat core: runs in its own high-priority task on core 1, independent of the UI.
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <stddef.h>
#include "control.h"
#include "schedule.h"
#include "datalog.h"
#include "alerts.h"
#include "service.h"

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
    int64_t override_until; // program overridden by hand until this epoch (0 = no override)
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
    // Boiler extras and totals since start
    bool press_valid;
    float pressure_bar;
    bool boiler_fault;
    uint8_t oem_fault;
    double gas_kwh_total;   // estimate from the modulation
    uint32_t ch_starts_total, dhw_starts_total;
    double burner_h_total;
    // Alerts (alerts.h)
    uint16_t alerts;        // bit per alert_id_t
    uint8_t alert_count;
    char alert_text[160];   // headline, "" = none
    // Service mode (service.h)
    bool svc_active, svc_running;
    uint8_t svc_routine;
    int svc_elapsed_s, svc_idle_left_s;
    char svc_step[80];
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

#define THERMO_HISTORY_LEN 1440  // 24 h at 1 / min (from the data log)

// 5-second samples of the last hour (chart: 5 min and 1 h views)
enum { THERMO_FINE_FLAME = 1, THERMO_FINE_HEAT = 2 };  // burner on / on for the radiators
typedef struct {
    uint32_t t;             // epoch, 0 = clock not set
    int16_t room_c10, target_c10, flow_c10;  // INT16_MIN = no value
    uint8_t flags;
} thermo_fine_t;
#define THERMO_FINE_LEN 720
int thermo_fine(thermo_fine_t *out, int max);  // oldest first

void thermo_start(void);
void thermo_get(thermo_state_t *out);

void thermo_set_setpoint(float c);
void thermo_adjust_setpoint(float delta);
// What the dial / -+ on the main page set: with the program running, a hand override until the next switch;
// otherwise the setpoint. A boost or away period ends first (an explicit temperature choice wins).
void thermo_set_target(float c);
void thermo_adjust_target(float delta);
void thermo_resume_program(void);  // drop the hand override now
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

// Alert settings (alerts.h); saved like the control settings.
void thermo_get_alert_config(alert_config_t *out);
void thermo_set_alert_config(const alert_config_t *c);

// KPIs over the last hour and the last 24 h (updated every minute).
void thermo_get_kpi(dl_kpi_t *h1, dl_kpi_t *h24);
// Minute log (7 days, RAM only): count and record i (0 = oldest).
int thermo_log_count(void);
bool thermo_log_get(int i, dl_rec_t *out);
int thermo_hour_count(void);
bool thermo_hour_get(int i, dl_hour_t *out);  // hourly summaries, 0 = oldest
// Wi-Fi signal for the log (set by the MQTT task; reading it here would block the control loop).
void thermo_set_rssi(int rssi);

// Service mode and commissioning routines. why gets a short reason on refusal.
bool thermo_svc_enter(bool on);
bool thermo_svc_start(svc_routine_t r, bool confirmed, char *why, size_t size);
void thermo_svc_cancel(void);
bool thermo_svc_manual(bool write, uint8_t id, uint16_t data, char *why, size_t size);
// Applies what a finished routine recommends (bus flags, flow_min_c). Writes what changed into msg.
bool thermo_svc_apply(svc_routine_t r, char *msg, size_t size);
void thermo_svc_get(svc_record_t *rec, char *manual_text, size_t size);  // either may be NULL
void thermo_get_svc_config(svc_config_t *out);
void thermo_set_svc_config(const svc_config_t *c);

// Copies up to max samples, oldest first; returns the count.
int thermo_history(thermo_sample_t *out, int max);

// Fault test: the control task stops for 1..60 s. With feed_watchdog the task watchdog stays quiet, so only the
// bus's command timeout acts (heating off after 10 s); without it the watchdog restarts the device after 10 s.
void thermo_test_hang(int seconds, bool feed_watchdog);
