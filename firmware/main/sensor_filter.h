#pragma once
// Room sensor filter (pure C, host-tested): plausibility, stuck-sensor detection, recovery steps, smoothing and the
// dead band for the shown values. The thermostat task reads the SHT40 every 2 s and feeds each result in here.
#include <stdbool.h>
#include <stdint.h>

#define SF_STUCK_N      150             // reads (2 s each): 5 minutes of bit-identical values = sensor stopped
#define SF_SMOOTH_ALPHA (2.0f / 30.0f)  // reading every 2 s -> ~30 s exponential average
#define SF_SHOW_BAND_C  0.08f           // shown temperature moves only past this from the smoothed value
#define SF_SHOW_BAND_RH 0.8f
#define SF_T_MIN        -20.0f          // plausible room readings (exclusive); -45 / 130 °C are all-0 / all-1 words
#define SF_T_MAX        60.0f

typedef enum { SF_ACT_NONE, SF_ACT_SOFT_RESET, SF_ACT_BUS_RESET } sf_action_t;

typedef struct {
    int64_t stale_us;       // no good reading for this long: sensor lost
    // outputs
    bool ok;                // a good reading within stale_us
    int age_s;              // seconds since the last good reading (-1 = none yet)
    float raw, room_c, humidity, room_shown, humidity_shown;
    uint32_t errors;
    int back_bad;           // set on the good reading that ends a run of bad ones: how many there were (else 0)
    int back_s;             //   and how long since the previous good reading (-1: none since the start)
    // state
    int64_t last_ok_us;     // 0 = none yet
    int fails_in_row;
    int same;
    int bad_run;            // bad reads since the last good one
    float last_t, last_h;
} sensor_filter_t;

void sf_init(sensor_filter_t *f, int stale_s);

// Step 1, the driver's result: counts failed reads in a row (-> *action: soft reset after 5, I2C bus reset after 15
// and every 15 after that) and turns a reading repeated bit-identically SF_STUCK_N times into a failure.
// Returns whether the reading may be used; *stuck_now is true once, on the read that crossed SF_STUCK_N.
bool sf_check(sensor_filter_t *f, bool read_ok, float t, float h, sf_action_t *action, bool *stuck_now);

// Step 2, the reading to use (offset already applied; ok=false for a failed read): plausibility, smoothing (restarted
// after a gap longer than stale_us, so an old value doesn't drag the new one), shown values, status.
// Returns false when the reading was rejected (counted in errors).
bool sf_accept(sensor_filter_t *f, int64_t now_us, bool ok, float t, float h);

// The offset changed by d: move every temperature with it, without restarting the average.
void sf_shift(sensor_filter_t *f, float d);
