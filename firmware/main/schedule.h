#pragma once
// Schedule layer above the control law (docs/research/10-recommendation-phase1.md, plan part B3/A4).
// Pure C (no ESP-IDF), host-tested. Called once per second by the thermostat task.
//
// Heating: weekly Comfort/Eco program, boost, away, summer mode → the room target and whether CH may run.
// Hot water (DHW): always / time windows / off, minimum-tank guard, piggy-back after a CH burn, DHW boost,
//   weekly pasteurisation through ID 56 (only while the boiler is not proven to ignore ID 56 writes).
// Without a valid clock: constant Comfort, DHW always enabled, no pasteurisation; away stays on until cancelled.
// Every feature has its own switch in sched_config_t; with every switch off the behaviour is the 0.2 one
// (constant setpoint, DHW always enabled, ID 56 never written).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCHED_CONFIG_VERSION 1
#define SCHED_POINTS 6           // switch points per day
#define SCHED_WINDOWS 3          // DHW windows (every day)
#define SCHED_UNUSED 0xFFFF      // unused switch point / window

typedef enum { SCHED_ECO = 0, SCHED_COMFORT = 1 } sched_level_t;

typedef struct {
    uint16_t min;                // minute of the day (0..1439), SCHED_UNUSED = empty
    uint8_t level;               // sched_level_t
} sched_point_t;

typedef struct {
    uint16_t start, end;         // minutes of the day; start == SCHED_UNUSED or start == end = unused;
                                 // end < start wraps past midnight
} sched_window_t;

typedef enum { DHW_ALWAYS = 0, DHW_WINDOWS = 1, DHW_OFF = 2 } sched_dhw_mode_t;

typedef struct {
    uint16_t version;
    // Weekly heating program
    bool prog_on;                // off = constant setpoint (the 0.2 behaviour)
    float eco_setback_k;         // Eco = Comfort − this
    sched_point_t prog[7][SCHED_POINTS];  // day 0 = Monday
    // Boost (time-limited higher target)
    bool boost_on;
    float boost_k;
    uint16_t boost_min;
    // Away / holiday
    bool away_on;
    float away_c;
    bool away_dhw_off;           // DHW off while away (the guard still applies)
    // Summer mode: CH off while the 24 h outdoor mean is warm (DHW keeps running)
    bool summer_on;
    float summer_c;
    float summer_hyst_k;
    // Hot water
    uint8_t dhw_mode;            // sched_dhw_mode_t
    sched_window_t dhw_win[SCHED_WINDOWS];
    bool guard_on;               // enable DHW outside the windows while the tank is below guard_c
    float guard_c;
    float guard_hyst_k;
    bool piggy_on;               // enable DHW for piggy_min after a CH burn (windows mode)
    uint16_t piggy_min;
    uint16_t dhw_boost_min;      // DHW boost length (0 = DHW boost off)
    bool dhw_set_on;             // write dhw_set_c to the boiler (ID 56); off = leave the panel value
    float dhw_set_c;
    // Weekly pasteurisation (ID 56)
    bool pasteur_on;
    uint8_t pasteur_day;         // 0 = Monday
    uint16_t pasteur_min;        // start, minute of the day
    float pasteur_c;
    uint16_t pasteur_hold_min;   // tank at pasteur_c − 2 for this long = done
    uint16_t pasteur_timeout_min;
} sched_config_t;

// ID 56 write check, reported by the bus (otb_core.h OTB_DHWSET_*)
typedef enum { SCHED_ID56_UNKNOWN = 0, SCHED_ID56_OK, SCHED_ID56_IGNORED, SCHED_ID56_REJECTED } sched_id56_t;

typedef struct {
    double now_s;                // monotonic seconds
    bool clock_ok;
    int wday;                    // 0 = Monday
    int min_of_day;
    int32_t day;                 // local day number (days since 1970), for once-a-week tracking
    int64_t epoch;               // wall clock (s), valid with clock_ok
    float comfort_c;             // the user's setpoint
    bool tank_valid;
    float tank_c;
    bool outside_valid;
    float outside_c;
    bool flame, dhw_active;
    bool dhw_set_valid;
    float dhw_set_read;          // the boiler's DHW setpoint (ID 56 read)
    uint8_t id56;                // sched_id56_t
} sched_in_t;

typedef enum {
    SCHED_SRC_CONSTANT = 0,      // program off (or no clock): the setpoint
    SCHED_SRC_COMFORT,
    SCHED_SRC_ECO,
    SCHED_SRC_BOOST,
    SCHED_SRC_AWAY,
    SCHED_SRC_SUMMER,            // CH off
} sched_src_t;

typedef enum {
    DHW_R_ALWAYS = 0, DHW_R_WINDOW, DHW_R_GUARD, DHW_R_PIGGY, DHW_R_BOOST, DHW_R_PASTEUR, DHW_R_NOCLOCK,
    DHW_R_CLOSED,                // windows mode, outside a window
    DHW_R_OFF, DHW_R_AWAY,
} sched_dhw_reason_t;

typedef enum {
    PASTEUR_NONE = 0, PASTEUR_OK, PASTEUR_TIMEOUT, PASTEUR_ID56_IGNORED, PASTEUR_NO_TANK, PASTEUR_NO_SETPOINT,
    PASTEUR_CANCELLED,
} sched_pasteur_result_t;

typedef struct {
    float target_c;              // room target for the control law
    bool ch_allowed;             // false in summer mode (the frost floor still works)
    uint8_t src;                 // sched_src_t
    int next_change_min;         // minutes until the next program change (-1 = none / program off)
    uint8_t next_level;          // sched_level_t at that change
    int boost_left_s;            // 0 = no boost
    bool away;
    int64_t away_until;          // 0 = until cancelled
    bool summer;
    bool outside_mean_valid;
    float outside_mean_c;        // 24 h mean
    bool dhw_enable;
    uint8_t dhw_reason;          // sched_dhw_reason_t
    int dhw_boost_left_s;
    float dhw_set;               // ID 56 value to write, 0 = don't write
    bool pasteur_running;
    uint8_t pasteur_result;      // sched_pasteur_result_t of the last run
    int64_t pasteur_last;        // epoch of the last result (0 = none)
} sched_out_t;

typedef struct {
    sched_config_t cfg;
    // Boost / away
    double boost_until_s, dhw_boost_until_s;
    bool away;
    int64_t away_until;
    // Summer: 24 hourly buckets of the outdoor temperature
    float out_sum[24];
    uint16_t out_n[24];
    int32_t out_hour[24];
    bool summer;
    // DHW
    bool guard;
    bool last_flame, last_ch_burn;
    double piggy_until_s;
    // Pasteurisation
    bool pasteur;
    int8_t pasteur_req;          // 1 = start requested, -1 = cancel requested
    double pasteur_wait_s;       // start pending since (waiting for the tank / ID 56 reading), -1 = none
    double pasteur_start_s, pasteur_hot_since_s;
    int32_t pasteur_done_day;
    float restore_c;             // ID 56 value to keep after a run (0 = none)
    sched_out_t out;
} sched_t;

void sched_default_config(sched_config_t *c);
void sched_sanitize(sched_config_t *c);
void sched_init(sched_t *s, const sched_config_t *cfg);
void sched_set_config(sched_t *s, const sched_config_t *cfg);  // keeps boost/away/pasteurisation state
const sched_out_t *sched_step(sched_t *s, const sched_in_t *in);

// User actions (now_s = monotonic seconds). They return false when the feature is switched off.
bool sched_boost(sched_t *s, double now_s, bool on);
bool sched_dhw_boost(sched_t *s, double now_s, bool on);
bool sched_away(sched_t *s, bool on, int64_t until);  // until = epoch, 0 = until cancelled
bool sched_pasteur_now(sched_t *s, bool on);           // start (or cancel) a pasteurisation run now

// Level of the program at day/minute (day 0 = Monday); the last point before it, wrapping back over the week.
sched_level_t sched_level_at(const sched_config_t *c, int wday, int min_of_day);

const char *sched_src_name(sched_src_t s);
const char *sched_dhw_reason_name(sched_dhw_reason_t r);
const char *sched_pasteur_name(sched_pasteur_result_t r);

// Named access to the settings (serial CFG, web). The weekly program is edited with the PROG helpers.
int sched_config_count(void);
const char *sched_config_name(int idx);
const char *sched_config_help(int idx);
// Field description (type, range) for generic editors; NULL past the end.
const struct ct_field *sched_config_field(int idx);
bool sched_config_get(const sched_config_t *c, int idx, char *out, size_t size);
bool sched_config_set(sched_config_t *c, const char *name, const char *value);
// One day of the program as text: "06:00=c 08:00=e 17:00=c 22:30=e" (c = Comfort, e = Eco).
void sched_prog_format(const sched_config_t *c, int wday, char *out, size_t size);
// Parses that format into the given day (empty text = no points). Returns false and leaves c unchanged on error.
bool sched_prog_parse(sched_config_t *c, int wday, const char *text);
