#include "schedule.h"
#include "cfgtable.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SUMMER_MIN_HOURS 12      // hours of outdoor data in the last 24 h before summer mode may switch
#define PASTEUR_START_WINDOW 60  // a missed start time is still taken up to this many minutes late
#define PASTEUR_BELOW_K 2.0f     // the tank counts as hot at pasteur_c - this (sensor position)
#define PASTEUR_WAIT_S 300.0     // a start waits this long for the tank and ID 56 readings (e.g. after a boot)

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ------------------------------------------------------------- config */

#define F(n, t, lo, hi, h) CT_FIELD(sched_config_t, n, t, lo, hi, h)
#define W(i) \
    {"dhw_win" #i "_start", CT_TIME_OPT, offsetof(sched_config_t, dhw_win[i - 1].start), 0, 1439, \
     "DHW window " #i ": start (HH:MM, - = unused)"}, \
    {"dhw_win" #i "_end", CT_TIME_OPT, offsetof(sched_config_t, dhw_win[i - 1].end), 0, 1439, \
     "DHW window " #i ": end (HH:MM, may pass midnight)"}
static const ct_field_t FIELDS[] = {
    F(prog_on, CT_BOOL, 0, 1, "weekly Comfort/Eco program on/off (off = constant setpoint); edit with PROG"),
    F(eco_setback_k, CT_FLOAT, 0.5f, 6, "Eco = Comfort - this (K)"),
    F(boost_on, CT_BOOL, 0, 1, "boost feature on/off"),
    F(boost_k, CT_FLOAT, 0.5f, 3, "boost: the higher of Comfort and the target in force + this (K)"),
    F(boost_min, CT_U16, 10, 240, "boost: length (min)"),
    F(away_on, CT_BOOL, 0, 1, "away/holiday feature on/off"),
    F(away_c, CT_FLOAT, 5, 20, "away: room target (°C)"),
    F(away_dhw_off, CT_BOOL, 0, 1, "away: hot water off (the tank guard still applies)"),
    F(summer_on, CT_BOOL, 0, 1, "summer mode on/off: CH off while the 24 h outdoor mean is warm"),
    F(summer_c, CT_FLOAT, 10, 25, "summer: CH off at or above this 24 h outdoor mean (°C)"),
    F(summer_hyst_k, CT_FLOAT, 0.5f, 5, "summer: CH back on below summer_c - this (K)"),
    F(dhw_mode, CT_U8, 0, 2, "hot water: 0 = always enabled, 1 = time windows, 2 = off"),
    W(1), W(2), W(3),
    F(guard_on, CT_BOOL, 0, 1, "tank guard on/off: enable hot water while the tank is below guard_c"),
    F(guard_c, CT_FLOAT, 30, 55, "tank guard: lowest tank temperature (°C)"),
    F(guard_hyst_k, CT_FLOAT, 1, 15, "tank guard: stays on until guard_c + this (K)"),
    F(piggy_on, CT_BOOL, 0, 1, "piggy-back on/off: enable hot water right after a heating burn (windows mode)"),
    F(piggy_min, CT_U16, 5, 60, "piggy-back: length (min)"),
    F(dhw_boost_min, CT_U16, 0, 240, "hot water boost: length (min, 0 = feature off)"),
    F(dhw_set_on, CT_BOOL, 0, 1, "write the tank setpoint to the boiler (ID 56) on/off; off = panel value"),
    F(dhw_set_c, CT_FLOAT, 35, 60, "tank setpoint written to the boiler (°C)"),
    F(pasteur_on, CT_BOOL, 0, 1, "weekly pasteurisation on/off (needs ID 56 writes to work)"),
    F(pasteur_day, CT_U8, 0, 6, "pasteurisation: day (0 = Monday)"),
    F(pasteur_min, CT_TIME, 0, 1439, "pasteurisation: start time (HH:MM)"),
    F(pasteur_c, CT_FLOAT, 60, 75, "pasteurisation: tank setpoint (°C)"),
    F(pasteur_hold_min, CT_U16, 5, 120, "pasteurisation: time at temperature (min)"),
    F(pasteur_timeout_min, CT_U16, 30, 360, "pasteurisation: give up after (min)"),
};
#define NFIELDS ((int)(sizeof(FIELDS) / sizeof(FIELDS[0])))

static void set_day(sched_config_t *c, int d, const sched_point_t *pts, int n)
{
    for (int i = 0; i < SCHED_POINTS; i++) {
        c->prog[d][i] = i < n ? pts[i] : (sched_point_t){SCHED_UNUSED, 0};
    }
}

void sched_default_config(sched_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->version = SCHED_CONFIG_VERSION;
    c->prog_on = false;
    c->eco_setback_k = 2;
    const sched_point_t wd[] = {{330, SCHED_COMFORT}, {1350, SCHED_ECO}};  // 05:30 – 22:30
    const sched_point_t we[] = {{420, SCHED_COMFORT}, {1380, SCHED_ECO}};  // 07:00 – 23:00
    for (int d = 0; d < 7; d++) {
        if (d < 5) {
            set_day(c, d, wd, 2);
        } else {
            set_day(c, d, we, 2);
        }
    }
    c->boost_on = true;
    c->boost_k = 1;
    c->boost_min = 60;
    c->away_on = true;
    c->away_c = 15;
    c->away_dhw_off = false;
    c->summer_on = false;
    c->summer_c = 18;
    c->summer_hyst_k = 2;
    c->dhw_mode = DHW_ALWAYS;
    c->dhw_win[0] = (sched_window_t){300, 420};   // 05:00 – 07:00
    c->dhw_win[1] = (sched_window_t){1050, 1260}; // 17:30 – 21:00
    c->dhw_win[2] = (sched_window_t){SCHED_UNUSED, SCHED_UNUSED};
    c->guard_on = true;
    c->guard_c = 40;
    c->guard_hyst_k = 5;
    c->piggy_on = false;
    c->piggy_min = 20;
    c->dhw_boost_min = 60;
    c->dhw_set_on = false;
    c->dhw_set_c = 50;
    c->pasteur_on = false;
    c->pasteur_day = 6;    // Sunday
    c->pasteur_min = 780;  // 13:00, after lunch, while people are awake (scald risk at the taps)
    c->pasteur_c = 65;
    c->pasteur_hold_min = 30;
    c->pasteur_timeout_min = 120;
}

static int cmp_point(const void *a, const void *b)
{
    const sched_point_t *x = a, *y = b;
    return (int)x->min - (int)y->min;
}

void sched_sanitize(sched_config_t *c)
{
    ct_sanitize(c, FIELDS, NFIELDS);
    for (int w = 0; w < SCHED_WINDOWS; w++) {
        if (c->dhw_win[w].start == SCHED_UNUSED || c->dhw_win[w].end == SCHED_UNUSED) {
            c->dhw_win[w] = (sched_window_t){SCHED_UNUSED, SCHED_UNUSED};
        }
    }
    for (int d = 0; d < 7; d++) {
        for (int i = 0; i < SCHED_POINTS; i++) {
            sched_point_t *p = &c->prog[d][i];
            if (p->min > 1439) {
                *p = (sched_point_t){SCHED_UNUSED, 0};
            } else if (p->level > SCHED_COMFORT && !sched_level_is_fixed(p->level)) {
                p->level = SCHED_COMFORT;
            }
        }
        qsort(c->prog[d], SCHED_POINTS, sizeof(sched_point_t), cmp_point);  // unused (0xFFFF) sort last
    }
    c->version = SCHED_CONFIG_VERSION;
}

int sched_config_count(void)
{
    return NFIELDS;
}

const char *sched_config_name(int idx)
{
    return idx >= 0 && idx < NFIELDS ? FIELDS[idx].name : NULL;
}

const struct ct_field *sched_config_field(int idx)
{
    return idx >= 0 && idx < NFIELDS ? &FIELDS[idx] : NULL;
}

const char *sched_config_help(int idx)
{
    return idx >= 0 && idx < NFIELDS ? FIELDS[idx].help : NULL;
}

static int parse_time(const char *s)
{
    int h, m;
    char tail;
    if (sscanf(s, "%d:%d%c", &h, &m, &tail) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
        return -1;
    }
    return h * 60 + m;
}

bool sched_config_get(const sched_config_t *c, int idx, char *out, size_t size)
{
    if (idx < 0 || idx >= NFIELDS) {
        return false;
    }
    ct_get(c, &FIELDS[idx], out, size);
    return true;
}

bool sched_config_set(sched_config_t *c, const char *name, const char *value)
{
    if (!ct_set(c, FIELDS, NFIELDS, name, value)) {
        return false;
    }
    sched_sanitize(c);
    return true;
}

void sched_prog_format(const sched_config_t *c, int wday, char *out, size_t size)
{
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < SCHED_POINTS && wday >= 0 && wday < 7; i++) {
        const sched_point_t *p = &c->prog[wday][i];
        if (p->min == SCHED_UNUSED) {
            continue;
        }
        char lv[8];
        if (sched_level_is_fixed(p->level)) {
            float c = p->level / 4.0f;
            snprintf(lv, sizeof(lv), c == (int)c ? "%.0f" : (c * 2 == (int)(c * 2) ? "%.1f" : "%.2f"), c);
        } else {
            snprintf(lv, sizeof(lv), "%c", p->level == SCHED_COMFORT ? 'c' : 'e');
        }
        int w = snprintf(out + n, size - n, "%s%02d:%02d=%s", n ? " " : "", p->min / 60, p->min % 60, lv);
        if (w < 0 || (size_t)w >= size - n) {
            break;
        }
        n += (size_t)w;
    }
}

bool sched_prog_parse(sched_config_t *c, int wday, const char *text)
{
    if (wday < 0 || wday > 6) {
        return false;
    }
    sched_point_t pts[SCHED_POINTS];
    int n = 0;
    char buf[160];
    snprintf(buf, sizeof(buf), "%s", text);
    for (char *save = NULL, *tok = strtok_r(buf, " ,", &save); tok; tok = strtok_r(NULL, " ,", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq || n >= SCHED_POINTS) {
            return false;
        }
        *eq = 0;
        int m = parse_time(tok);
        const char *v = eq + 1;
        uint8_t level;
        if (m < 0) {
            return false;
        }
        if ((v[0] == 'c' || v[0] == 'C') && !v[1]) {
            level = SCHED_COMFORT;
        } else if ((v[0] == 'e' || v[0] == 'E') && !v[1]) {
            level = SCHED_ECO;
        } else {
            if (v[0] == 't' || v[0] == 'T') {
                v++;
            }
            char *end;
            float c = strtof(v, &end);
            if (end == v || *end || c < 5 || c > 30) {
                return false;
            }
            level = sched_level_from_c(c);
        }
        pts[n++] = (sched_point_t){(uint16_t)m, level};
    }
    set_day(c, wday, pts, n);
    sched_sanitize(c);
    return true;
}

/* ------------------------------------------------------------ program */

uint8_t sched_level_from_c(float c)
{
    int l = (int)lroundf(c * 4);
    return (uint8_t)(l < SCHED_FIXED_MIN ? SCHED_FIXED_MIN : (l > SCHED_FIXED_MAX ? SCHED_FIXED_MAX : l));
}

void sched_level_text(uint8_t level, char *out, size_t size)
{
    if (sched_level_is_fixed(level)) {
        if (level % 4) {
            snprintf(out, size, level % 2 ? "%.2f" : "%.1f", level / 4.0f);
        } else {
            snprintf(out, size, "%.0f", level / 4.0f);
        }
    } else {
        snprintf(out, size, "%s", level == SCHED_COMFORT ? "comfort" : "eco");
    }
}

float sched_level_temp(uint8_t level, float comfort_c, float eco_setback_k)
{
    if (sched_level_is_fixed(level)) {
        return level / 4.0f;
    }
    return level == SCHED_COMFORT ? comfort_c : comfort_c - eco_setback_k;
}

sched_level_t sched_level_at(const sched_config_t *c, int wday, int min_of_day)
{
    for (int back = 0; back <= 7; back++) {
        int d = ((wday - back) % 7 + 7) % 7;
        int best = -1;
        for (int i = 0; i < SCHED_POINTS; i++) {
            const sched_point_t *p = &c->prog[d][i];
            if (p->min == SCHED_UNUSED || (back == 0 && p->min > min_of_day)) {
                continue;
            }
            if (best < 0 || p->min >= c->prog[d][best].min) {
                best = i;
            }
        }
        if (best >= 0) {
            return (sched_level_t)c->prog[d][best].level;
        }
    }
    return SCHED_COMFORT;  // empty program
}

// Minutes until the level changes from cur (-1 = never within a week).
static int next_change(const sched_config_t *c, int wday, int min_of_day, sched_level_t cur, uint8_t *next)
{
    for (int ahead = 0; ahead <= 7; ahead++) {
        int d = (wday + ahead) % 7;
        for (int i = 0; i < SCHED_POINTS; i++) {  // sorted by sanitize
            const sched_point_t *p = &c->prog[d][i];
            if (p->min == SCHED_UNUSED || (ahead == 0 && p->min <= min_of_day)) {
                continue;
            }
            if (p->level != cur) {
                *next = p->level;
                return ahead * 1440 + p->min - min_of_day;
            }
        }
    }
    return -1;
}

static bool in_window(const sched_window_t *w, int m)
{
    if (w->start == SCHED_UNUSED || w->end == SCHED_UNUSED || w->start == w->end) {
        return false;
    }
    if (w->start < w->end) {
        return m >= w->start && m < w->end;
    }
    return m >= w->start || m < w->end;  // past midnight
}

/* --------------------------------------------------------------- state */

void sched_init(sched_t *s, const sched_config_t *cfg)
{
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    sched_sanitize(&s->cfg);
    for (int i = 0; i < 24; i++) {
        s->out_hour[i] = -1;
    }
    s->pasteur_done_day = -1;
    s->pasteur_hot_since_s = -1;
    s->pasteur_wait_s = -1;
    s->out.next_change_min = -1;
    s->seen_day = -1;
}

void sched_set_config(sched_t *s, const sched_config_t *cfg)
{
    s->cfg = *cfg;
    sched_sanitize(&s->cfg);
}

bool sched_boost(sched_t *s, double now_s, bool on)
{
    if (on && !s->cfg.boost_on) {
        return false;
    }
    s->boost_until_s = on ? now_s + s->cfg.boost_min * 60.0 : 0;
    return true;
}

bool sched_override(sched_t *s, bool on, float target_c)
{
    if (on && !s->cfg.prog_on) {
        return false;
    }
    s->ovr = on;
    s->ovr_c = clampf(target_c, 5, 30);
    s->ovr_end_set = false;  // the end is fixed at the next step, from the program's next switch
    s->ovr_until = 0;
    return true;
}

bool sched_dhw_boost(sched_t *s, double now_s, bool on)
{
    if (on && (s->cfg.dhw_boost_min == 0 || s->cfg.dhw_mode == DHW_OFF)) {
        return false;
    }
    s->dhw_boost_until_s = on ? now_s + s->cfg.dhw_boost_min * 60.0 : 0;
    return true;
}

bool sched_away(sched_t *s, bool on, int64_t until)
{
    if (on && !s->cfg.away_on) {
        return false;
    }
    s->away = on;
    s->away_until = on ? until : 0;
    return true;
}

bool sched_pasteur_now(sched_t *s, bool on)
{
    if (on && (!s->cfg.pasteur_on || s->cfg.dhw_mode == DHW_OFF)) {
        return false;
    }
    s->pasteur_req = on ? 1 : -1;
    return true;
}

static void pasteur_finish(sched_t *s, const sched_in_t *in, sched_pasteur_result_t r)
{
    s->pasteur = false;
    s->pasteur_hot_since_s = -1;
    s->out.pasteur_result = r;
    s->out.pasteur_last = in->clock_ok ? in->epoch : 0;
}

// Tries to start a run; returns false while it should be retried (readings not there yet).
static bool pasteur_start(sched_t *s, const sched_in_t *in)
{
    const sched_config_t *k = &s->cfg;
    if (in->id56 == SCHED_ID56_IGNORED || in->id56 == SCHED_ID56_REJECTED) {
        pasteur_finish(s, in, PASTEUR_ID56_IGNORED);  // the boiler keeps its panel value: panel routine needed
        return true;
    }
    float restore = k->dhw_set_on ? k->dhw_set_c : (in->dhw_set_valid ? in->dhw_set_read : 0);
    if (s->restore_c > 0 && !k->dhw_set_on) {
        restore = s->restore_c;  // the previous run is still giving the old value back
    }
    if (!in->tank_valid || restore <= 0) {
        if (s->pasteur_wait_s < 0) {
            s->pasteur_wait_s = in->now_s;
        }
        if (in->now_s - s->pasteur_wait_s < PASTEUR_WAIT_S) {
            return false;
        }
        pasteur_finish(s, in, !in->tank_valid ? PASTEUR_NO_TANK : PASTEUR_NO_SETPOINT);
        return true;
    }
    if (restore >= k->pasteur_c) {
        pasteur_finish(s, in, PASTEUR_NO_SETPOINT);  // nothing sensible to return to afterwards
        return true;
    }
    s->restore_c = restore;
    s->pasteur = true;
    s->pasteur_start_s = in->now_s;
    s->pasteur_hot_since_s = -1;
    return true;
}

static void step_pasteur(sched_t *s, const sched_in_t *in)
{
    const sched_config_t *k = &s->cfg;
    if (s->pasteur_req < 0) {
        if (s->pasteur || s->pasteur_wait_s >= 0) {
            pasteur_finish(s, in, PASTEUR_CANCELLED);
        }
        s->pasteur_req = 0;
        s->pasteur_wait_s = -1;
    }
    if (!s->pasteur && k->pasteur_on && k->dhw_mode != DHW_OFF && in->clock_ok && in->wday == k->pasteur_day &&
        in->min_of_day >= k->pasteur_min && in->min_of_day < k->pasteur_min + PASTEUR_START_WINDOW &&
        s->pasteur_done_day != in->day) {
        s->pasteur_done_day = in->day;
        s->pasteur_req = 1;
    }
    if (s->pasteur_req > 0 && !s->pasteur) {
        if (!k->pasteur_on || k->dhw_mode == DHW_OFF) {
            s->pasteur_req = 0;
            s->pasteur_wait_s = -1;
        } else if (pasteur_start(s, in)) {
            s->pasteur_req = 0;
            s->pasteur_wait_s = -1;
        }
    }
    if (!s->pasteur) {
        return;
    }
    if (!k->pasteur_on || k->dhw_mode == DHW_OFF) {
        pasteur_finish(s, in, PASTEUR_CANCELLED);
    } else if (in->id56 == SCHED_ID56_IGNORED || in->id56 == SCHED_ID56_REJECTED) {
        pasteur_finish(s, in, PASTEUR_ID56_IGNORED);
    } else if (in->now_s - s->pasteur_start_s >= k->pasteur_timeout_min * 60.0) {
        pasteur_finish(s, in, PASTEUR_TIMEOUT);
    } else if (in->tank_valid && in->tank_c >= k->pasteur_c - PASTEUR_BELOW_K) {
        if (s->pasteur_hot_since_s < 0) {
            s->pasteur_hot_since_s = in->now_s;
        } else if (in->now_s - s->pasteur_hot_since_s >= k->pasteur_hold_min * 60.0) {
            pasteur_finish(s, in, PASTEUR_OK);
        }
    } else {
        s->pasteur_hot_since_s = -1;
    }
}

static void step_outside(sched_t *s, const sched_in_t *in)
{
    int32_t h = (int32_t)(in->now_s / 3600);
    if (in->outside_valid) {
        int b = h % 24;
        if (s->out_hour[b] != h) {
            s->out_hour[b] = h;
            s->out_sum[b] = 0;
            s->out_n[b] = 0;
        }
        if (s->out_n[b] < 60000) {
            s->out_sum[b] += in->outside_c;
            s->out_n[b]++;
        }
    }
    float sum = 0;
    int hours = 0;
    for (int b = 0; b < 24; b++) {
        if (s->out_n[b] && s->out_hour[b] > h - 24) {
            sum += s->out_sum[b] / s->out_n[b];
            hours++;
        }
    }
    s->out.outside_mean_valid = hours >= SUMMER_MIN_HOURS;
    s->out.outside_mean_c = hours ? sum / hours : 0;
    if (!s->cfg.summer_on) {
        s->summer = false;
    } else if (s->out.outside_mean_valid) {
        if (s->out.outside_mean_c >= s->cfg.summer_c) {
            s->summer = true;
        } else if (s->out.outside_mean_c < s->cfg.summer_c - s->cfg.summer_hyst_k) {
            s->summer = false;
        }
    }  // no outdoor data: keep the last decision
}

const sched_out_t *sched_step(sched_t *s, const sched_in_t *in)
{
    const sched_config_t *k = &s->cfg;
    sched_out_t *o = &s->out;
    double now = in->now_s;

    // Features switched off while active
    if (!k->boost_on) {
        s->boost_until_s = 0;
    }
    if (!k->away_on) {
        s->away = false;
    }
    if (k->dhw_boost_min == 0 || k->dhw_mode == DHW_OFF) {
        s->dhw_boost_until_s = 0;
    }
    if (s->away && s->away_until && in->clock_ok && in->epoch >= s->away_until) {
        s->away = false;  // holiday over
    }

    step_outside(s, in);

    // Heating target
    float comfort = in->comfort_c;
    o->target_c = comfort;
    o->src = SCHED_SRC_CONSTANT;
    o->next_change_min = -1;
    if (k->prog_on && in->clock_ok) {
        // The clock going back (the repeated hour when DST ends, or a small correction) holds the program at the
        // latest minute it has run at, so a switch point in that hour runs once, not twice.
        int mod = in->min_of_day;
        if (in->day == s->seen_day && mod < s->seen_min && s->seen_min - mod <= 60) {
            mod = s->seen_min;
        } else {
            s->seen_day = in->day;
            s->seen_min = (int16_t)mod;
        }
        sched_level_t l = sched_level_at(k, in->wday, mod);
        o->src = sched_level_is_fixed(l) ? SCHED_SRC_FIXED : (l == SCHED_COMFORT ? SCHED_SRC_COMFORT : SCHED_SRC_ECO);
        o->target_c = sched_level_temp(l, comfort, k->eco_setback_k);
        o->next_change_min = next_change(k, in->wday, mod, l, &o->next_level);
        if (s->ovr && !s->ovr_end_set) {
            int end = mod + (o->next_change_min > 0 ? o->next_change_min : 24 * 60);
            s->ovr_end_day = in->day + end / 1440;
            s->ovr_end_min = (int16_t)(end % 1440);
            s->ovr_end_set = true;
        }
        int left = s->ovr ? (s->ovr_end_day - in->day) * 1440 + s->ovr_end_min - mod : 0;
        if (s->ovr && left <= 0) {
            s->ovr = false;  // the program's next switch: back to the program
        }
        if (s->ovr) {
            o->target_c = s->ovr_c;
            o->src = SCHED_SRC_MANUAL;
            s->ovr_until = in->epoch - in->epoch % 60 + left * 60LL;
        }
    } else {
        s->ovr = false;  // program off or no clock: the dial sets the setpoint itself
    }
    o->override_until = s->ovr ? s->ovr_until : 0;
    float before_away = o->target_c;  // program / override / setpoint: what a boost adds to
    if (s->away) {
        o->target_c = k->away_c;
        o->src = SCHED_SRC_AWAY;
    }
    bool boost = s->boost_until_s > now;
    o->boost_left_s = boost ? (int)(s->boost_until_s - now) : 0;
    if (boost) {
        // An explicit request: wins over away and summer; never below the target in force (a 24 °C program point)
        o->target_c = fmaxf(before_away, comfort) + k->boost_k;
        o->src = SCHED_SRC_BOOST;
    }
    o->ch_allowed = !s->summer || boost;
    if (!o->ch_allowed) {
        o->src = SCHED_SRC_SUMMER;
    }
    o->target_c = clampf(o->target_c, 5, 30);
    o->away = s->away;
    o->away_until = s->away ? s->away_until : 0;
    o->summer = s->summer;

    // Burn edges for piggy-back: a CH burn is one that did not run for the tank at any time
    if (in->flame && !s->last_flame) {
        s->last_ch_burn = !in->dhw_active;
    }
    if (in->flame && in->dhw_active) {
        s->last_ch_burn = false;
    }
    if (!in->flame && s->last_flame && s->last_ch_burn && k->piggy_on) {
        s->piggy_until_s = now + k->piggy_min * 60.0;
    }
    s->last_flame = in->flame;
    if (!k->piggy_on) {
        s->piggy_until_s = 0;
    }

    // Tank guard
    if (k->guard_on && in->tank_valid) {
        if (in->tank_c < k->guard_c) {
            s->guard = true;
        } else if (in->tank_c >= k->guard_c + k->guard_hyst_k) {
            s->guard = false;
        }
    } else {
        s->guard = false;
    }

    step_pasteur(s, in);
    // After a run (feature dhw_set_on off): give ID 56 back to the panel once the boiler shows the old value
    if (!s->pasteur && s->restore_c > 0 && !k->dhw_set_on && in->dhw_set_valid &&
        fabsf(in->dhw_set_read - s->restore_c) < 0.5f) {
        s->restore_c = 0;
    }

    // Hot water enable
    bool dboost = s->dhw_boost_until_s > now;
    o->dhw_boost_left_s = dboost ? (int)(s->dhw_boost_until_s - now) : 0;
    bool en;
    sched_dhw_reason_t why;
    if (k->dhw_mode == DHW_OFF) {
        en = false, why = DHW_R_OFF;
    } else if (s->pasteur) {
        en = true, why = DHW_R_PASTEUR;
    } else if (dboost) {
        en = true, why = DHW_R_BOOST;
    } else if (s->away && k->away_dhw_off) {
        en = s->guard, why = s->guard ? DHW_R_GUARD : DHW_R_AWAY;
    } else if (k->dhw_mode == DHW_ALWAYS) {
        en = true, why = DHW_R_ALWAYS;
    } else if (!in->clock_ok) {
        en = true, why = DHW_R_NOCLOCK;
    } else {
        bool win = false;
        for (int w = 0; w < SCHED_WINDOWS; w++) {
            win |= in_window(&k->dhw_win[w], in->min_of_day);
        }
        if (win) {
            en = true, why = DHW_R_WINDOW;
        } else if (s->guard) {
            en = true, why = DHW_R_GUARD;
        } else if (s->piggy_until_s > now) {
            en = true, why = DHW_R_PIGGY;
        } else {
            en = false, why = DHW_R_CLOSED;
        }
    }
    o->dhw_enable = en;
    o->dhw_reason = why;
    o->pasteur_running = s->pasteur;
    o->dhw_set = s->pasteur ? k->pasteur_c : (k->dhw_set_on ? k->dhw_set_c : s->restore_c);
    return o;
}

/* --------------------------------------------------------------- names */

const char *sched_src_name(sched_src_t s)
{
    static const char *n[] = {"constant", "comfort", "eco", "boost", "away", "summer", "program", "manual"};
    return s <= SCHED_SRC_MANUAL ? n[s] : "?";
}

const char *sched_dhw_reason_name(sched_dhw_reason_t r)
{
    static const char *n[] = {"always", "window", "guard", "piggy", "boost", "pasteur", "noclock", "closed", "off",
                              "away"};
    return r <= DHW_R_AWAY ? n[r] : "?";
}

const char *sched_pasteur_name(sched_pasteur_result_t r)
{
    static const char *n[] = {"none", "ok", "timeout", "id56-ignored", "no-tank", "no-setpoint", "cancelled"};
    return r <= PASTEUR_CANCELLED ? n[r] : "?";
}
