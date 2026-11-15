#include "alerts.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "cfgtable.h"
#include "schedule.h"

#define NEVER (-1.0)

#define F(n, t, lo, hi, h) CT_FIELD(alert_config_t, n, t, lo, hi, h)
static const ct_field_t FIELDS[] = {
    F(fault_on, CT_BOOL, 0, 1, "alert: boiler fault on/off"),
    F(pressure_on, CT_BOOL, 0, 1, "alert: low water pressure on/off"),
    F(pressure_min_bar, CT_FLOAT, 0.3f, 2, "low pressure: below this (bar)"),
    F(leak_on, CT_BOOL, 0, 1, "alert: pressure falling (slow leak) on/off"),
    F(leak_bar_24h, CT_FLOAT, 0.05f, 1, "leak: pressure drop over 24 h (bar)"),
    F(link_on, CT_BOOL, 0, 1, "alert: OpenTherm link lost on/off"),
    F(link_s, CT_U16, 5, 3600, "link lost: after this long (s)"),
    F(sensor_on, CT_BOOL, 0, 1, "alert: room sensor lost on/off"),
    F(sensor_s, CT_U16, 30, 3600, "sensor lost: after this long without a reading (s)"),
    F(short_on, CT_BOOL, 0, 1, "alert: short cycling on/off"),
    F(short_starts_h, CT_U8, 2, 30, "short cycling: at least this many CH starts in the last hour"),
    F(short_median_min, CT_FLOAT, 1, 15, "short cycling: with a median burn shorter than this (min)"),
    F(cold_on, CT_BOOL, 0, 1, "alert: room not reaching the target on/off"),
    F(cold_k, CT_FLOAT, 0.3f, 5, "cold room: below target − this (K)"),
    F(cold_h, CT_FLOAT, 0.5f, 24, "cold room: for this long (h)"),
    F(pasteur_on, CT_BOOL, 0, 1, "alert: pasteurisation did not finish on/off"),
    F(clock_on, CT_BOOL, 0, 1, "alert: no valid clock on/off"),
    F(clock_s, CT_U16, 60, 3600, "no clock: after this long (s)"),
};
#define NFIELDS ((int)(sizeof(FIELDS) / sizeof(FIELDS[0])))

static const char *NAMES[ALERT_COUNT] = {"fault", "low_pressure", "pressure_falling", "link_lost", "sensor_lost",
                                         "short_cycling", "cold_room", "pasteurisation", "no_clock"};
static const char *TITLES[ALERT_COUNT] = {"Boiler fault", "Low water pressure", "Water pressure falling",
                                          "Boiler not answering", "Room sensor lost", "Short cycling",
                                          "Room not reaching target", "Pasteurisation failed", "Clock not set"};

const char *alert_name(alert_id_t id)
{
    return id < ALERT_COUNT ? NAMES[id] : "?";
}

const char *alert_title(alert_id_t id)
{
    return id < ALERT_COUNT ? TITLES[id] : "?";
}

void alert_default_config(alert_config_t *c)
{
    *c = (alert_config_t){
        .version = ALERT_CONFIG_VERSION,
        .fault_on = true,
        .pressure_on = true, .pressure_min_bar = 1.0f,
        .leak_on = true, .leak_bar_24h = 0.3f,
        .link_on = true, .link_s = 60,
        .sensor_on = true, .sensor_s = 60,
        .short_on = true, .short_starts_h = 4, .short_median_min = 3,
        .cold_on = true, .cold_k = 1.0f, .cold_h = 3,
        .pasteur_on = true,
        .clock_on = true, .clock_s = 600,
    };
}

void alert_sanitize(alert_config_t *c)
{
    ct_sanitize(c, FIELDS, NFIELDS);
    c->version = ALERT_CONFIG_VERSION;
}

int alert_config_count(void)
{
    return NFIELDS;
}

const char *alert_config_name(int idx)
{
    return idx >= 0 && idx < NFIELDS ? FIELDS[idx].name : NULL;
}

const char *alert_config_help(int idx)
{
    return idx >= 0 && idx < NFIELDS ? FIELDS[idx].help : NULL;
}

bool alert_config_get(const alert_config_t *c, int idx, char *out, size_t size)
{
    if (idx < 0 || idx >= NFIELDS) {
        return false;
    }
    ct_get(c, &FIELDS[idx], out, size);
    return true;
}

bool alert_config_set(alert_config_t *c, const char *name, const char *value)
{
    return ct_set(c, FIELDS, NFIELDS, name, value);
}

void alerts_init(alerts_t *a, const alert_config_t *cfg)
{
    memset(a, 0, sizeof(*a));
    a->cfg = *cfg;
    alert_sanitize(&a->cfg);
    for (int i = 0; i < ALERT_COUNT; i++) {
        a->cond_since[i] = a->clear_since[i] = NEVER;
    }
}

void alerts_set_config(alerts_t *a, const alert_config_t *cfg)
{
    a->cfg = *cfg;
    alert_sanitize(&a->cfg);
}

// Condition + delay -> active; cleared ALERT_CLEAR_S after the condition is gone (or at once when disabled).
static void eval(alerts_t *a, alert_id_t id, bool enabled, bool cond, double delay_s, double now)
{
    alert_out_t *o = &a->out;
    if (!enabled) {
        o->active[id] = false;
        a->cond_since[id] = a->clear_since[id] = NEVER;
        return;
    }
    if (cond) {
        a->clear_since[id] = NEVER;
        if (a->cond_since[id] < 0) {
            a->cond_since[id] = now;
        }
        if (!o->active[id] && now - a->cond_since[id] >= delay_s) {
            o->active[id] = true;
            o->since_s[id] = now;
        }
    } else {
        a->cond_since[id] = NEVER;
        if (o->active[id]) {
            if (a->clear_since[id] < 0) {
                a->clear_since[id] = now;
            }
            if (now - a->clear_since[id] >= ALERT_CLEAR_S) {
                o->active[id] = false;
                a->clear_since[id] = NEVER;
            }
        }
    }
}

const alert_out_t *alerts_step(alerts_t *a, const alert_in_t *in)
{
    const alert_config_t *k = &a->cfg;
    double now = in->now_s;

    // Hourly pressure samples for the leak trend (25 = now and 24 h back)
    if (in->press_valid && now >= a->press_next_s) {
        a->press_next_s = now + 3600;
        if (a->press_n == 25) {
            memmove(a->press_h, a->press_h + 1, 24 * sizeof(float));
            a->press_n = 24;
        }
        a->press_h[a->press_n++] = in->pressure_bar;
    }
    bool leak = false;
    if (a->press_n == 25 && in->press_valid) {
        float max = a->press_h[0];
        for (int i = 1; i < 25; i++) {
            max = fmaxf(max, a->press_h[i]);
        }
        leak = max - in->pressure_bar >= k->leak_bar_24h;
    }

    bool up = in->link_up;
    eval(a, ALERT_FAULT, k->fault_on, up && in->boiler_fault, 5, now);
    eval(a, ALERT_PRESSURE, k->pressure_on, up && in->press_valid && in->pressure_bar < k->pressure_min_bar, 30, now);
    eval(a, ALERT_LEAK, k->leak_on, up && leak, 0, now);
    eval(a, ALERT_LINK, k->link_on, in->link_known && !in->link_up, k->link_s, now);
    eval(a, ALERT_SENSOR, k->sensor_on, !in->sensor_ok, k->sensor_s, now);
    eval(a, ALERT_SHORT_CYCLE, k->short_on,
         in->ch_starts_1h >= k->short_starts_h && in->ch_burns_1h >= 2 && in->median_burn_1h_min < k->short_median_min,
         0, now);
    eval(a, ALERT_COLD, k->cold_on,
         in->heating_allowed && in->sensor_ok && in->room_c < in->target_c - k->cold_k, k->cold_h * 3600, now);
    bool pfail = in->pasteur_result == PASTEUR_TIMEOUT || in->pasteur_result == PASTEUR_ID56_IGNORED ||
                 in->pasteur_result == PASTEUR_NO_TANK || in->pasteur_result == PASTEUR_NO_SETPOINT;
    eval(a, ALERT_PASTEUR, k->pasteur_on, pfail, 0, now);
    eval(a, ALERT_CLOCK, k->clock_on, !in->clock_ok, k->clock_s, now);

    // Count and the headline (array order = importance)
    alert_out_t *o = &a->out;
    o->count = 0;
    o->text[0] = 0;
    for (int i = 0; i < ALERT_COUNT; i++) {
        if (!o->active[i]) {
            continue;
        }
        if (!o->count) {
            if (i == ALERT_FAULT && in->fault_valid) {
                snprintf(o->text, sizeof(o->text), "%s (code %u)", TITLES[i], in->oem_fault);
            } else if (i == ALERT_PRESSURE) {
                snprintf(o->text, sizeof(o->text), "%s (%.1f bar)", TITLES[i], in->pressure_bar);
            } else {
                snprintf(o->text, sizeof(o->text), "%s", TITLES[i]);
            }
        }
        o->count++;
    }
    if (o->count > 1) {
        size_t n = strlen(o->text);
        snprintf(o->text + n, sizeof(o->text) - n, " (+%d more)", o->count - 1);
    }
    return o;
}
