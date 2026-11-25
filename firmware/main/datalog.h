#pragma once
// Data log and efficiency KPIs (docs/research/09, plan A6). Pure C (no ESP-IDF), host-tested.
//
// Fed once per second. Keeps:
//   - one record per minute in a caller-supplied ring (PSRAM on the device; 24 h = 1440 records);
//   - one event per burner burn (start, length, CH or DHW, mean return and modulation);
//   - one summary per clock hour (35 days), for the week chart;
// and computes the KPIs over a window (last hour, last 24 h): CH starts per hour, median CH burn, % short
// burns, % burner time with the return below the condensing limit, mean modulation, % time at minimum,
// comfort degree-hours and an estimated gas input from the modulation.
// The device copies every new record to flash (histstore.c) and replays them after a restart with the
// datalog_restore_* functions. MQTT/VictoriaMetrics keep the long raw history.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DL_NO_VALUE INT16_MIN

enum {
    DL_F_SENSOR_OK = 1u << 0, DL_F_BOILER_UP = 1u << 1, DL_F_CH_EN = 1u << 2, DL_F_DHW_EN = 1u << 3,
    DL_F_FAULT = 1u << 4, DL_F_HEAT_MODE = 1u << 5, DL_F_CLOCK = 1u << 6, DL_F_SIM = 1u << 7,
};

// One second of input (filled by the thermostat task).
#define DL_BACKSTEP_MAX_S 7200  // a clock step back by up to this much: wait for it to catch up

typedef struct {
    int64_t epoch;               // 0 = clock not set
    bool sensor_ok;
    float room_c, humidity, target_c, setpoint_c;
    bool heat_mode, ch_allowed;  // heating may run (mode Heat, not summer)
    bool boiler_up, sim;
    bool flame, dhw_active, fault;
    bool ch_enable, dhw_enable;
    float tset_c, req_c, p, i;
    bool flow_valid, ret_valid, mod_valid, press_valid, tank_valid, out_valid;
    float flow_c, return_c, modulation, pressure_bar, tank_c, outside_c;
    uint8_t ctrl_state, sched_src, dhw_reason;
    int8_t rssi;
    uint32_t ot_errors;          // running total
    float cap_kw, min_kw;        // boiler maximum and minimum input (ID 15), 0 = unknown
    bool svc;                    // service mode on: burns are tests, kept out of the burn statistics
} dl_sample_t;

// Marker for minutes and burns in service mode. A value, not a bit: the byte was padding in older records.
#define DL_MARK_SVC 0x5C

// One minute (56 bytes; 7 days = 565 KB of PSRAM).
typedef struct {
    int64_t t;                   // epoch at the end of the minute, 0 = no clock
    uint32_t up_min;             // minutes since start
    int16_t room_c100;           // DL_NO_VALUE = no reading
    int16_t target_c10, setpoint_c10, tset_c10, req_c10;
    int16_t flow_c10, ret_c10, tank_c10, out_c10, press_c100;
    int16_t p_c10, i_c10;
    uint8_t humidity, mod;       // mean modulation while the flame was on
    uint8_t flame_s, dhw_s;      // seconds with flame, of those for the tank
    uint8_t cond_s;              // CH flame seconds with the return below the condensing limit
    uint8_t ch_starts, dhw_starts;
    uint8_t flags;               // DL_F_*
    uint8_t ctrl_state, sched_src, dhw_reason;
    int8_t rssi;
    uint8_t ot_err;              // errors this minute (255 = 255 or more)
    uint8_t min_s;               // CH flame seconds at minimum modulation
    uint16_t wh;                 // estimated gas input this minute (Wh)
    uint8_t mark;                // DL_MARK_SVC = service mode during this minute (was padding: size unchanged)
} dl_rec_t;

typedef struct {
    uint32_t start_s;            // seconds since start (0 for burns restored from before the last start)
    uint32_t t;                  // epoch at the start, 0 = clock not set
    uint16_t dur_s;
    bool dhw;                    // the burn heated the tank at any time
    uint8_t ret_c, mod;          // means over the burn
    uint8_t mark;                // DL_MARK_SVC = a service-test burn (was padding: size unchanged)
} dl_burn_t;

// One clock hour (needs the clock; hours without it are not summarised).
typedef struct {
    uint32_t t;                  // epoch of the start of the hour
    int16_t room_c10, room_min_c10, room_max_c10;  // DL_NO_VALUE = no reading
    int16_t target_c10;
    int16_t flow_c10;            // mean while the burner was on, DL_NO_VALUE = no burn
    uint16_t wh;                 // estimated gas input
    uint8_t heat_min, dhw_min;   // burner minutes for the radiators / the tank (rounded)
    uint8_t ch_starts;
    uint8_t minutes;             // minutes of data in the hour
} dl_hour_t;

typedef struct {
    float window_h;              // requested window
    float hours;                 // data actually covered
    uint16_t ch_starts, dhw_starts;
    float ch_starts_h;
    uint16_t ch_burns;           // completed CH burns in the window
    float median_burn_min;       // CH burns
    float short_pct;             // CH burns shorter than DL_SHORT_BURN_S
    float burner_h, dhw_burner_h;
    float cond_pct;              // CH burner time with the return below DL_COND_RETURN_C
    float mean_mod;              // CH burner time
    float min_mod_pct;           // CH burner time at minimum modulation
    float comfort_kh;            // degree-hours below target − DL_COMFORT_BAND_K while heating may run
    float over_kh;               // degree-hours above target + DL_COMFORT_BAND_K
    float gas_kwh;               // estimate
    float mean_room, mean_target, mean_flow, mean_return;
    float ot_err;                // OpenTherm errors in the window
    uint16_t svc_min;            // minutes in service mode (tests): left out of starts, burns and comfort
} dl_kpi_t;

#define DL_SHORT_BURN_S 600
#define DL_COND_RETURN_C 55.0f
#define DL_MIN_MOD_PCT 5.0f
#define DL_COMFORT_BAND_K 0.5f

typedef struct {
    dl_rec_t *recs;
    int nrecs, head, count;
    dl_burn_t *burns;
    int nburns, bhead, bcount;
    dl_hour_t *hours;
    int nhours, hhead, hcount;
    uint32_t rec_seq, burn_seq, hour_seq;  // records added so far (histstore writes the new ones)
    uint32_t skipped;                      // minute records left out after the clock was set back
    // Hour in progress
    uint32_t h_t;
    int h_n, h_nroom, h_nflow;
    double h_room, h_target, h_flow, h_wh, h_heat_s, h_dhw_s;
    int16_t h_min, h_max;
    int h_starts;
    // Current minute
    uint32_t sec;                // seconds since start
    int n;                       // samples in this minute
    double sum_room, sum_hum, sum_target, sum_flow, sum_ret, sum_tank, sum_out, sum_press, mod_sum;
    int n_room, n_flow, n_ret, n_tank, n_out, n_press;
    int flame_s, dhw_s, cond_s, min_s, ch_starts, dhw_starts;
    bool svc;                    // service mode at any second of this minute
    double wh;
    uint32_t ot_err0;
    bool have_err0;
    // Burn in progress
    bool flame;
    uint32_t burn_start, burn_t;
    bool burn_dhw, burn_svc;
    double burn_ret, burn_mod;
    int burn_n;
    // Totals since start
    uint32_t total_ch_starts, total_dhw_starts;
    double total_kwh, total_burner_s;
} dl_t;

void datalog_init(dl_t *d, dl_rec_t *recs, int nrecs, dl_burn_t *burns, int nburns, dl_hour_t *hours, int nhours);
// After a restart, oldest first: puts saved entries back (they do not change the totals since start).
void datalog_restore_rec(dl_t *d, const dl_rec_t *r);
void datalog_restore_burn(dl_t *d, const dl_burn_t *b);
void datalog_restore_hour(dl_t *d, const dl_hour_t *h);
// Once per second. Returns true when a minute record was completed.
bool datalog_tick(dl_t *d, const dl_sample_t *s);
// KPIs over the last window_h hours (from the minute records and burn events).
void datalog_kpi(const dl_t *d, float window_h, dl_kpi_t *k);
// Record i, 0 = oldest. Returns false past the end.
bool datalog_get(const dl_t *d, int i, dl_rec_t *out);
int datalog_count(const dl_t *d);
bool datalog_get_burn(const dl_t *d, int i, dl_burn_t *out);  // 0 = oldest
int datalog_burn_count(const dl_t *d);
bool datalog_get_hour(const dl_t *d, int i, dl_hour_t *out);  // 0 = oldest
int datalog_hour_count(const dl_t *d);
// CSV
const char *datalog_csv_header(void);
int datalog_csv_row(const dl_rec_t *r, char *out, size_t size);
// Estimated burner input (kW) for a modulation level: OpenTherm ID 17 is relative between the minimum and
// the maximum input; unknown capacity -> the Victrix Tera 24 values.
float datalog_power_kw(float mod_pct, float cap_kw, float min_kw);
