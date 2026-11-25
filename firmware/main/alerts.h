#pragma once
// Alerts (plan B4): evaluated once per second, shown on the device and sent to Home Assistant.
// Pure C (no ESP-IDF), host-tested. Every alert has its own switch and thresholds in alert_config_t.
// An alert turns on when its condition has held for its delay and off after the condition has been gone for
// ALERT_CLEAR_S (no flapping).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ALERT_CONFIG_VERSION 2  // 2: link_poor_on / link_poor_pct appended
#define ALERT_CLEAR_S 60

typedef enum {
    ALERT_FAULT = 0,     // boiler fault bit (ID 0), with the OEM code (ID 5)
    ALERT_PRESSURE,      // CH water pressure (ID 18) below the limit
    ALERT_LEAK,          // pressure fell by more than the limit over 24 h
    ALERT_LINK,          // OpenTherm link lost
    ALERT_SENSOR,        // room sensor not answering
    ALERT_SHORT_CYCLE,   // many short CH burns in the last hour
    ALERT_COLD,          // room below target for hours while heating may run
    ALERT_PASTEUR,       // the last pasteurisation did not finish
    ALERT_CLOCK,         // no valid clock (schedule falls back to Comfort)
    ALERT_LINK_POOR,     // the boiler answers, but many frames fail (loose wire, interference)
    ALERT_COUNT
} alert_id_t;

typedef struct {
    uint16_t version;
    bool fault_on;
    bool pressure_on;
    float pressure_min_bar;
    bool leak_on;
    float leak_bar_24h;
    bool link_on;
    uint16_t link_s;
    bool sensor_on;
    uint16_t sensor_s;
    bool short_on;
    uint8_t short_starts_h;      // at least this many CH starts in the last hour ...
    float short_median_min;      // ... with a median burn shorter than this
    bool cold_on;
    float cold_k;                // room below target − this ...
    float cold_h;                // ... for this long
    bool pasteur_on;
    bool clock_on;
    uint16_t clock_s;
    bool link_poor_on;
    uint8_t link_poor_pct;       // failed frames over the last 5 minutes, at least this share (%)
} alert_config_t;

typedef struct {
    double now_s;
    bool boiler_fault;
    uint8_t oem_fault;
    bool fault_valid;            // the OEM code was read
    bool press_valid;
    float pressure_bar;
    bool link_up;
    bool link_known;             // false for the first 5 s (before the bus gave up or got an answer); never-answered = lost
    uint16_t frames_5m, fails_5m;  // frames to the boiler and failed ones in the last ~5 minutes
    bool sensor_ok;
    uint16_t ch_starts_1h;
    float median_burn_1h_min;
    uint16_t ch_burns_1h;
    bool heating_allowed;        // mode Heat and not summer
    float room_c, target_c;
    uint8_t pasteur_result;      // sched_pasteur_result_t
    bool clock_ok;
} alert_in_t;

typedef struct {
    bool active[ALERT_COUNT];
    double since_s[ALERT_COUNT];
    int count;                   // active alerts
    char text[160];              // the most important active one, for the screen ("" = none)
} alert_out_t;

typedef struct {
    alert_config_t cfg;
    double cond_since[ALERT_COUNT], clear_since[ALERT_COUNT];
    // Pressure history: one sample per hour
    float press_h[25];
    int press_n;
    double press_next_s;
    alert_out_t out;
} alerts_t;

void alert_default_config(alert_config_t *c);
void alert_sanitize(alert_config_t *c);
// A saved blob of an older version (len bytes): its fields kept, the newer ones at their defaults. false = unknown.
bool alert_config_migrate(alert_config_t *out, const void *blob, size_t len);
void alerts_init(alerts_t *a, const alert_config_t *cfg);
void alerts_set_config(alerts_t *a, const alert_config_t *cfg);
const alert_out_t *alerts_step(alerts_t *a, const alert_in_t *in);
const char *alert_name(alert_id_t id);   // short id, e.g. "low_pressure" (MQTT / JSON)
const char *alert_title(alert_id_t id);  // e.g. "Low water pressure"

int alert_config_count(void);
const char *alert_config_name(int idx);
const char *alert_config_help(int idx);
// Field description (type, range) for generic editors; NULL past the end.
const struct ct_field *alert_config_field(int idx);
bool alert_config_get(const alert_config_t *c, int idx, char *out, size_t size);
bool alert_config_set(alert_config_t *c, const char *name, const char *value);
