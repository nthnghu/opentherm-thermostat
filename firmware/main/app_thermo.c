// Thermostat app: swipeable pages. Page 1 = room temperature, clock, setpoint; page 2 = history chart;
// page 3 = schedule, page 4 = boiler, page 5 = settings (ui_pages.c). Every page has the Apps button; the
// dots at the bottom jump to a page, and swiping up from the bottom edge returns to page 1.
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "board.h"
#include "ui.h"
#include "ui_theme.h"
#include "thermostat.h"
#include "timekeep.h"
#include "ui_pages.h"
#include "core/lv_obj_event_private.h"  // lv_hit_test_info_t (the dial's knob-only touch area)


static lv_obj_t *s_scr, *s_tv;
LV_FONT_DECLARE(font_room_75);  // tools/gen_font.py: digits . - ° only

static lv_obj_t *s_time, *s_date, *s_room, *s_hum, *s_arc, *s_set, *s_mode_btn, *s_mode_lbl, *s_info;
static lv_obj_t *s_src, *s_boost_btn, *s_away_btn, *s_prog_btn, *s_alert, *s_alert_lbl, *s_room_dot;
static lv_obj_t *s_dots[5];

/* ------------------------------------------------------------- main page */

// The dial and -/+ set the target: with the program on, a hand override until its next switch.
static void minus_cb(lv_event_t *e)
{
    thermo_adjust_target(-0.5f);
}

static void plus_cb(lv_event_t *e)
{
    thermo_adjust_target(0.5f);
}

static void arc_cb(lv_event_t *e)
{
    thermo_set_target(lv_arc_get_value(s_arc) / 2.0f);
}

// The dial takes a touch only on its knob (with some room for a fingertip). Anywhere else on it, a touch goes
// to the page below, so a swipe across the dial changes the page and can no longer drag the target along.
#define DIAL_GRAB_PX 46
static void arc_hit_cb(lv_event_t *e)
{
    lv_hit_test_info_t *info = lv_event_get_param(e);
    lv_area_t a;
    lv_obj_get_coords(s_arc, &a);
    float f = (float)(lv_arc_get_value(s_arc) - lv_arc_get_min_value(s_arc)) /
              (float)(lv_arc_get_max_value(s_arc) - lv_arc_get_min_value(s_arc));
    float ang = (135 + 270 * f) * (float)M_PI / 180;
    int kx = (a.x1 + a.x2) / 2 + (int)lroundf(169 * cosf(ang)), ky = (a.y1 + a.y2) / 2 + (int)lroundf(169 * sinf(ang));
    int dx = info->point->x - kx, dy = info->point->y - ky;
    info->res = dx * dx + dy * dy <= DIAL_GRAB_PX * DIAL_GRAB_PX;
}

static void boost_cb(lv_event_t *e)
{
    thermo_state_t st;
    thermo_get(&st);
    thermo_boost(st.boost_left_s <= 0);
}

static void away_cb(lv_event_t *e)
{
    thermo_state_t st;
    thermo_get(&st);
    thermo_away(!st.away, 0);
}

static void prog_cb(lv_event_t *e)
{
    thermo_resume_program();
}

// "until 17:00", or "until Tue 06:00" when it is not today
static void until_text(int64_t epoch, char *out, size_t size)
{
    time_t u = (time_t)epoch, now = time(NULL);
    struct tm t, n;
    localtime_r(&u, &t);
    localtime_r(&now, &n);
    if (t.tm_yday == n.tm_yday && t.tm_year == n.tm_year) {
        strftime(out, size, "until %H:%M", &t);
    } else {
        strftime(out, size, "until %a %H:%M", &t);
    }
}

// What sets the target now, in words and colour, shown in the dial above the target.
static void source_text(const thermo_state_t *st, char *out, size_t size, uint32_t *col)
{
    char until[32] = "";
    if (st->next_change_min >= 0) {
        until_text(time(NULL) + st->next_change_min * 60LL, until, sizeof(until));
    }
    *col = 0x90A4AE;
    switch (st->sched_src) {
    case SCHED_SRC_COMFORT:
        snprintf(out, size, "Comfort %s", until);
        *col = 0xFF7043;
        break;
    case SCHED_SRC_ECO:
        snprintf(out, size, "Eco %s", until);
        *col = 0x42A5F5;
        break;
    case SCHED_SRC_FIXED:
        snprintf(out, size, "Program %s", until);
        *col = 0x9575CD;
        break;
    case SCHED_SRC_MANUAL:
        until_text(st->override_until, until, sizeof(until));
        snprintf(out, size, "Manual %s", until);
        *col = 0xFFB300;
        break;
    case SCHED_SRC_BOOST:
        snprintf(out, size, "Boost, %d min left", (st->boost_left_s + 59) / 60);
        *col = 0xFF5722;
        break;
    case SCHED_SRC_AWAY:
        if (st->away_until) {
            until_text(st->away_until, until, sizeof(until));
            snprintf(out, size, "Away %s", until);
        } else {
            snprintf(out, size, "Away");
        }
        *col = 0x26A69A;
        break;
    case SCHED_SRC_SUMMER:
        snprintf(out, size, "Summer: heating off");
        break;
    default:
        snprintf(out, size, "Set to");
        break;
    }
}

static void mode_cb(lv_event_t *e)
{
    thermo_state_t st;
    thermo_get(&st);
    thermo_set_mode(st.mode == THERMO_MODE_HEAT ? THERMO_MODE_OFF : THERMO_MODE_HEAT);
}

static void info_cb(lv_event_t *e)
{
    app_thermo_show_page(3);  // the boiler page, with the alerts
}

static void apps_cb(lv_event_t *e)
{
    ui_go_launcher();
}

static void update_main(void)
{
    thermo_state_t st;
    thermo_get(&st);

    if (timekeep_valid()) {
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        char d[32];
        strftime(d, sizeof(d), "%a %d %b", &lt);
        lv_label_set_text_fmt(s_time, "%02d:%02d", lt.tm_hour, lt.tm_min);
        lv_label_set_text(s_date, d);
    } else {
        lv_label_set_text(s_time, "--:--");
        lv_label_set_text(s_date, "clock not set");
    }

    lv_obj_set_style_text_color(s_room, ui_c(st.sensor_age_s >= 6 ? 0x607D8B : 0xE6EDF3), 0);
    if (st.sensor_ok) {
        lv_label_set_text_fmt(s_room, "%.1f°", st.room_shown);
        lv_label_set_text_fmt(s_hum, "%.0f %% humidity", st.humidity_shown);
    } else {
        lv_label_set_text(s_room, "--.-°");
        lv_label_set_text_fmt(s_hum, "#%06lX sensor error#", (unsigned long)ui_hex(UI_ERR));
    }

    bool heat = st.mode == THERMO_MODE_HEAT;
    char src[48];
    uint32_t src_col;
    source_text(&st, src, sizeof(src), &src_col);
    lv_label_set_text(s_src, heat ? src : "Heating off");
    lv_obj_set_style_text_color(s_src, ui_c(heat ? src_col : 0x90A4AE), 0);
    lv_label_set_text_fmt(s_set, "%.1f °C", st.target_c);
    if (!lv_obj_has_state(s_arc, LV_STATE_PRESSED)) {
        lv_arc_set_value(s_arc, (int)lroundf(st.target_c * 2));
    }
    // Boost / Away / back-to-program buttons (hidden when the feature is switched off)
    sched_config_t sc;
    thermo_get_sched_config(&sc);
    bool boost = st.boost_left_s > 0;
    lv_obj_set_hidden(s_boost_btn, !sc.boost_on || !heat);
    lv_label_set_text(lv_obj_get_child(s_boost_btn, 0), boost ? LV_SYMBOL_CLOSE " Boost" : LV_SYMBOL_UP " Boost");
    lv_obj_set_style_bg_color(s_boost_btn, ui_c(boost ? 0xFF5722 : 0x455A64), 0);
    lv_obj_set_hidden(s_away_btn, !sc.away_on || !heat);
    lv_label_set_text(lv_obj_get_child(s_away_btn, 0), st.away ? LV_SYMBOL_HOME " I'm back" : LV_SYMBOL_GPS " Away");
    lv_obj_set_style_bg_color(s_away_btn, ui_c(st.away ? 0x26A69A : 0x455A64), 0);
    lv_obj_set_hidden(s_prog_btn, st.sched_src != SCHED_SRC_MANUAL);
    lv_label_set_text(s_mode_lbl, heat ? "Heating: on" : "Heating: off");
    lv_obj_set_style_bg_color(s_mode_btn, ui_c(heat ? 0xD84315 : 0x455A64), 0);
    lv_obj_set_style_arc_color(s_arc, ui_c(heat ? 0xFF7043 : 0x607D8B), LV_PART_INDICATOR);

    // Boiler line: what it does now, in plain words
    char boiler[80];
    const char *sim = st.boiler_sim ? " (simulated)" : "";
    if (!st.boiler_online) {
        snprintf(boiler, sizeof(boiler), "%s", st.boiler_sim ? "simulated boiler offline" : "no boiler connected");
    } else if (st.flame) {
        snprintf(boiler, sizeof(boiler), "water %.0f °C  •  power %.0f %%%s", st.flow_c,
                 st.modulation > 100 ? 100 : st.modulation, sim);
    } else {
        snprintf(boiler, sizeof(boiler), "water %.0f °C  •  burner off%s", st.flow_c, sim);
    }
    ctrl_config_t cfg;
    thermo_get_ctrl_config(&cfg);
    char loss[64];
    if (cfg.loss_mode == CTRL_LOSS_FIXED) {
        snprintf(loss, sizeof(loss), "fallback heating %.0f °C, %u %%", cfg.loss_flow_c, cfg.loss_duty_pct);
    } else {
        snprintf(loss, sizeof(loss), "%s", cfg.loss_mode == CTRL_LOSS_HEAT_OFF ? "heating off" : "last setting kept");
    }
    // Alert chip (tap: Boiler page) above the status line; hidden when there is nothing to report
    char chip[200] = "";
    uint32_t chip_col = UI_WARN;
    if (st.svc_running) {
        snprintf(chip, sizeof(chip), LV_SYMBOL_SETTINGS " Service: %s - %s", svc_routine_title(st.svc_routine), st.svc_step);
    } else if (!st.sensor_ok) {
        snprintf(chip, sizeof(chip), LV_SYMBOL_WARNING " Room sensor not answering - %s", loss);
        chip_col = UI_ERR;
    } else if (st.sensor_age_s >= 6) {
        snprintf(chip, sizeof(chip), LV_SYMBOL_WARNING " Room sensor silent for %d s - %s in %d s", st.sensor_age_s, loss,
                 THERMO_SENSOR_STALE_S - st.sensor_age_s);
    } else if (st.alert_count) {
        snprintf(chip, sizeof(chip), LV_SYMBOL_WARNING " %s", st.alert_text);
    }
    lv_obj_set_hidden(s_alert, !chip[0]);
    lv_label_set_text(s_alert_lbl, chip);
    lv_obj_set_style_text_color(s_alert_lbl, ui_c(chip_col), 0);
    lv_obj_set_y(s_info, chip[0] ? 666 : 640);

    if (st.demand) {
        const char *why = st.ctrl_state == CTRL_ST_HOLD      ? " (keeping the small flame)"
                          : st.ctrl_state == CTRL_ST_DUTY_ON ? " (long burn, low demand)"
                          : st.ctrl_state == CTRL_ST_FROST   ? " (frost protection)"
                                                             : "";
        ui_label_fmt(s_info, "#FF7043 Heating# to %.0f °C%s\n%s", st.flow_setpoint, why, boiler);
    } else if (st.ctrl_state == CTRL_ST_DUTY_OFF && heat) {
        ui_label_fmt(s_info, "Pausing (low demand: fewer, longer burns)\n%s", boiler);
    } else if (!heat && cfg.frost_on) {
        ui_label_fmt(s_info, "Idle (frost protection below %.0f °C)\n%s", cfg.frost_room_c, boiler);
    } else {
        ui_label_fmt(s_info, "Idle\n%s", boiler);
    }

    // Where the room is on the dial (the knob is the target)
    float f = (st.room_shown - THERMO_SP_MIN) / (THERMO_SP_MAX - THERMO_SP_MIN);
    f = f < 0 ? 0 : (f > 1 ? 1 : f);
    float ang = (135 + 270 * f) * (float)M_PI / 180;
    lv_obj_set_pos(s_room_dot, 180 - 7 + (int)lroundf(169 * cosf(ang)), 180 - 7 + (int)lroundf(169 * sinf(ang)));
    lv_obj_set_hidden(s_room_dot, !st.sensor_ok);
}

static void build_main(lv_obj_t *p)
{
    s_time = lv_label_create(p);
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_48, 0);
    lv_obj_set_pos(s_time, 20, 8);
    s_date = lv_label_create(p);
    lv_obj_set_style_text_color(s_date, ui_c(0x90A4AE), 0);
    lv_obj_set_pos(s_date, 24, 66);

    lv_obj_t *apps = ui_button(p, LV_SYMBOL_BARS " Apps", ui_c(0x37474F), apps_cb, NULL);
    lv_obj_set_size(apps, 120, 56);
    lv_obj_align(apps, LV_ALIGN_TOP_RIGHT, -16, 16);

    s_arc = lv_arc_create(p);
    lv_obj_set_size(s_arc, 360, 360);
    lv_obj_align(s_arc, LV_ALIGN_TOP_MID, 0, 96);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, (int)(THERMO_SP_MIN * 2), (int)(THERMO_SP_MAX * 2));
    lv_obj_set_style_arc_width(s_arc, 22, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 22, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s_arc, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(s_arc, arc_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_set_adv_hittest(s_arc, true);
    lv_obj_add_event_cb(s_arc, arc_hit_cb, LV_EVENT_HIT_TEST, NULL);
    lv_obj_set_scroll_chain(s_arc, false);

    lv_obj_t *rl = lv_label_create(s_arc);
    lv_obj_set_style_text_color(rl, ui_c(0x90A4AE), 0);
    lv_label_set_text(rl, "Room");
    lv_obj_align(rl, LV_ALIGN_CENTER, 0, -84);
    s_room = lv_label_create(s_arc);
    lv_obj_set_style_text_font(s_room, &font_room_75, 0);  // drawn at full size: scaling a 48 px font blurred it
    lv_obj_align(s_room, LV_ALIGN_CENTER, 0, -24);
    s_hum = lv_label_create(s_arc);
    lv_label_set_recolor(s_hum, true);
    lv_obj_set_style_text_color(s_hum, ui_c(0x4FC3F7), 0);
    lv_obj_align(s_hum, LV_ALIGN_CENTER, 0, 38);
    s_src = lv_label_create(s_arc);  // what sets the target: Comfort/Eco/Program/Manual until .., Boost, Away
    lv_obj_set_style_text_color(s_src, ui_c(0x90A4AE), 0);
    lv_obj_align(s_src, LV_ALIGN_CENTER, 0, 82);
    s_set = lv_label_create(s_arc);
    lv_obj_set_style_text_font(s_set, &lv_font_montserrat_28, 0);
    lv_obj_align(s_set, LV_ALIGN_CENTER, 0, 114);

    // The room temperature on the dial: a dot on the ring (the knob is the target)
    s_room_dot = lv_obj_create(s_arc);
    lv_obj_remove_style_all(s_room_dot);
    lv_obj_set_size(s_room_dot, 14, 14);
    lv_obj_set_style_radius(s_room_dot, 7, 0);
    lv_obj_set_style_bg_opa(s_room_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_room_dot, ui_c(UI_TEXT), 0);
    lv_obj_set_style_border_width(s_room_dot, 2, 0);
    lv_obj_set_style_border_color(s_room_dot, ui_c(UI_BG), 0);
    lv_obj_set_clickable(s_room_dot, false);

    lv_obj_t *minus = ui_button(p, LV_SYMBOL_MINUS, ui_c(0x1565C0), minus_cb, NULL);
    lv_obj_set_size(minus, 104, 76);
    lv_obj_set_pos(minus, 20, 466);
    lv_obj_set_style_text_font(lv_obj_get_child(minus, 0), &lv_font_montserrat_40, 0);
    lv_obj_t *plus = ui_button(p, LV_SYMBOL_PLUS, ui_c(0xD84315), plus_cb, NULL);
    lv_obj_set_size(plus, 104, 76);
    lv_obj_set_pos(plus, 356, 466);
    lv_obj_set_style_text_font(lv_obj_get_child(plus, 0), &lv_font_montserrat_40, 0);
    s_mode_btn = ui_button(p, "", ui_c(0xD84315), mode_cb, NULL);
    lv_obj_set_size(s_mode_btn, 208, 76);
    lv_obj_set_pos(s_mode_btn, 136, 466);
    s_mode_lbl = lv_obj_get_child(s_mode_btn, 0);
    lv_obj_set_style_text_font(s_mode_lbl, &lv_font_montserrat_28, 0);

    // Boost / Away / back to the program: a row the shown buttons share; hidden buttons leave no gap
    lv_obj_t *row = lv_obj_create(p);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 440, 62);
    lv_obj_set_pos(row, 20, 552);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_scrollable(row, false);
    s_boost_btn = ui_button(row, LV_SYMBOL_UP " Boost", ui_c(0x455A64), boost_cb, NULL);
    lv_obj_set_height(s_boost_btn, 62);
    lv_obj_set_flex_grow(s_boost_btn, 1);
    s_away_btn = ui_button(row, LV_SYMBOL_GPS " Away", ui_c(0x455A64), away_cb, NULL);
    lv_obj_set_height(s_away_btn, 62);
    lv_obj_set_flex_grow(s_away_btn, 1);
    s_prog_btn = ui_button(row, LV_SYMBOL_REFRESH " Program", ui_c(0x5E35B1), prog_cb, NULL);
    lv_obj_set_height(s_prog_btn, 62);
    lv_obj_set_flex_grow(s_prog_btn, 1);

    // Alert chip, then two status lines: what the thermostat does, what the boiler does. Both open the Boiler page.
    s_alert = ui_button(p, "", ui_c(UI_SURFACE), info_cb, NULL);
    lv_obj_set_size(s_alert, 440, 38);
    lv_obj_set_pos(s_alert, 20, 622);
    lv_obj_set_style_shadow_width(s_alert, 0, 0);
    s_alert_lbl = lv_obj_get_child(s_alert, 0);
    lv_obj_set_width(s_alert_lbl, 420);
    lv_label_set_long_mode(s_alert_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_alert_lbl, LV_TEXT_ALIGN_CENTER, 0);
    s_info = lv_label_create(p);
    lv_label_set_recolor(s_info, true);
    lv_obj_set_width(s_info, 440);
    lv_obj_set_height(s_info, 48);
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_info, 20, 640);
    lv_obj_set_clickable(s_info, true);
    lv_obj_add_event_cb(s_info, info_cb, LV_EVENT_CLICKED, NULL);
}

/* ------------------------------------------------------------ chart page */

// Ranges: 5 min and 1 h from the 5-second ring, 6 h and 24 h from the minute log, a week from the hourly
// summaries. Each range is averaged into npts buckets; gaps (no data, clock not set) stay empty.
enum { R_5M = 0, R_1H, R_6H, R_24H, R_WEEK, R_COUNT };
static const int RANGE_S[R_COUNT] = {300, 3600, 6 * 3600, 86400, 7 * 86400};
static const int RANGE_PTS[R_COUNT] = {60, 180, 180, 180, 168};
#define MAX_PTS 180

#define CH_X 62  // plot area on the page
#define CH_Y 160
#define CH_W 356
#define CH_H 400
#define X_TICKS 9

static lv_obj_t *s_chart, *s_heat, *s_chart_info, *s_yscale, *s_wscale, *s_xscale, *s_range, *s_wlbl, *s_vgrid[X_TICKS];
static lv_chart_series_t *s_ser_room, *s_ser_target, *s_ser_flow;
static int s_rng = R_24H;
static bool s_water_off;  // the water line is hidden (tap the legend)
static void *s_cbuf;  // PSRAM scratch for the samples of one range
static char s_ylbl[16][8], s_xlbl[X_TICKS][24];
static const char *s_ytxt[17], *s_xtxt[X_TICKS + 1];
// Time labels sit on round clock times: every minute, 15 min, hour, 6 hours, day
static const int TICK_S[R_COUNT] = {60, 900, 3600, 6 * 3600, 86400};

typedef struct {
    double room, flow, heat;
    int nroom, nflow, nheat;
    int16_t target;
} bucket_t;

static void bucket_add(bucket_t *b, int16_t room_c10, int16_t target_c10, int16_t flow_c10, float heat_frac)
{
    if (room_c10 != INT16_MIN) {
        b->room += room_c10, b->nroom++;
    }
    if (flow_c10 != INT16_MIN) {
        b->flow += flow_c10, b->nflow++;
    }
    if (target_c10 != INT16_MIN) {
        b->target = target_c10;
    }
    b->heat += heat_frac, b->nheat++;
}

// Bucket of a sample: by its time when the clock is set, otherwise by its age (index from the newest).
static int bucket_of(uint32_t t, int age_s, time_t now, bool clock, int range_s, int npts)
{
    long a = (clock && t) ? (long)(now - (time_t)t) : (long)age_s;
    if (a < 0 || a >= range_s) {
        return -1;
    }
    return npts - 1 - (int)(a * npts / range_s);
}

static void update_chart(void)
{
    const int range_s = RANGE_S[s_rng], npts = RANGE_PTS[s_rng];
    bucket_t *bk = calloc(npts, sizeof(bucket_t));
    if (!bk || !s_cbuf) {
        free(bk);
        return;
    }
    for (int i = 0; i < npts; i++) {
        bk[i].target = INT16_MIN;
    }
    bool clock = timekeep_valid();
    time_t now = time(NULL);

    if (s_rng <= R_1H) {
        thermo_fine_t *f = s_cbuf;
        int n = thermo_fine(f, THERMO_FINE_LEN);
        for (int i = 0; i < n; i++) {
            int k = bucket_of(f[i].t, (n - 1 - i) * 5, now, clock, range_s, npts);
            if (k >= 0) {
                bucket_add(&bk[k], f[i].room_c10, f[i].target_c10, f[i].flow_c10, (f[i].flags & THERMO_FINE_HEAT) ? 1 : 0);
            }
        }
    } else if (s_rng <= R_24H) {
        int n = thermo_log_count();
        dl_rec_t r;
        for (int i = 0; i < n; i++) {
            if (!thermo_log_get(i, &r)) {
                continue;
            }
            int k = bucket_of((uint32_t)r.t, (n - 1 - i) * 60, now, clock, range_s, npts);
            if (k >= 0) {
                bucket_add(&bk[k], r.room_c100 == DL_NO_VALUE ? INT16_MIN : (int16_t)(r.room_c100 / 10), r.target_c10,
                           r.flow_c10 == DL_NO_VALUE ? INT16_MIN : r.flow_c10, (r.flame_s - r.dhw_s) / 60.0f);
            }
        }
    } else {
        int n = thermo_hour_count();
        dl_hour_t h;
        for (int i = 0; i < n; i++) {
            if (!thermo_hour_get(i, &h)) {
                continue;
            }
            int k = bucket_of(h.t + 3599, (n - 1 - i) * 3600, now, clock, range_s, npts);
            if (k >= 0) {
                bucket_add(&bk[k], h.room_c10 == DL_NO_VALUE ? INT16_MIN : h.room_c10, h.target_c10,
                           h.flow_c10 == DL_NO_VALUE ? INT16_MIN : h.flow_c10, h.heat_min / 60.0f);
            }
        }
    }

    // Series and the °C range of room/target
    static int32_t room[MAX_PTS], target[MAX_PTS], flow[MAX_PTS], heat[MAX_PTS];
    int lo = INT32_MAX, hi = INT32_MIN, heat_n = 0, heat_on = 0;
    float cur_room = NAN, cur_target = NAN, cur_flow = NAN;
    for (int i = 0; i < npts; i++) {
        room[i] = bk[i].nroom ? (int32_t)lround(bk[i].room / bk[i].nroom) : LV_CHART_POINT_NONE;
        target[i] = bk[i].target != INT16_MIN ? bk[i].target : LV_CHART_POINT_NONE;
        flow[i] = bk[i].nflow ? (int32_t)lround(bk[i].flow / bk[i].nflow) : LV_CHART_POINT_NONE;
        heat[i] = bk[i].nheat ? (int32_t)lround(100 * bk[i].heat / bk[i].nheat) : 0;
        if (bk[i].nheat) {
            heat_n++;
            heat_on += heat[i] > 0;
        }
        for (int k = 0; k < 2; k++) {
            int32_t v = k ? target[i] : room[i];
            if (v != LV_CHART_POINT_NONE) {
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
            }
        }
        if (room[i] != LV_CHART_POINT_NONE) {
            cur_room = room[i] / 10.0f;
        }
        if (target[i] != LV_CHART_POINT_NONE) {
            cur_target = target[i] / 10.0f;
        }
        if (flow[i] != LV_CHART_POINT_NONE) {
            cur_flow = flow[i] / 10.0f;
        }
    }
    free(bk);
    if (lo > hi) {
        lo = 180, hi = 240;
    }
    // Round outwards to 0.5 °C with a little margin, at least 2 °C tall
    int ylo = (int)floor((lo - 2) / 5.0) * 5, yhi = (int)ceil((hi + 2) / 5.0) * 5;
    if (yhi - ylo < 20) {
        int mid = (ylo + yhi) / 2;
        ylo = (int)floor((mid - 10) / 5.0) * 5;
        yhi = ylo + 20;
    }
    int span = yhi - ylo;
    int step = span <= 40 ? 5 : (span <= 80 ? 10 : (span <= 160 ? 20 : 50));  // 0.5, 1, 2 or 5 °C per tick
    int ticks = (span + step - 1) / step + 1;
    for (int i = 0; i < ticks; i++) {
        int v = ylo + i * step;
        snprintf(s_ylbl[i], sizeof(s_ylbl[i]), v % 10 ? "%.1f" : "%.0f", v / 10.0);
        s_ytxt[i] = s_ylbl[i];
    }
    s_ytxt[ticks] = NULL;
    lv_scale_set_range(s_yscale, ylo, ylo + (ticks - 1) * step);
    lv_scale_set_total_tick_count(s_yscale, ticks);
    lv_scale_set_major_tick_every(s_yscale, 1);
    lv_scale_set_text_src(s_yscale, s_ytxt);

    // Series colours follow the theme; the water line is paler so that the room line leads
    lv_chart_set_series_color(s_chart, s_ser_room, ui_c(UI_VALUE));
    lv_chart_set_series_color(s_chart, s_ser_target, ui_c(UI_WARN));
    lv_chart_set_series_color(s_chart, s_ser_flow, lv_color_hex(ui_theme_is_dark() ? 0x8C4A4A : 0xEDB4B4));
    lv_chart_hide_series(s_chart, s_ser_flow, s_water_off);
    lv_obj_set_hidden(s_wscale, s_water_off);
    lv_obj_set_hidden(s_wlbl, s_water_off);

    lv_chart_set_point_count(s_chart, npts);
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, ylo, ylo + (ticks - 1) * step);
    lv_chart_set_hor_div_line_count(s_chart, ticks);
    lv_chart_set_series_values(s_chart, s_ser_room, room, npts);
    lv_chart_set_series_values(s_chart, s_ser_target, target, npts);
    lv_chart_set_series_values(s_chart, s_ser_flow, flow, npts);
    lv_chart_refresh(s_chart);

    // Heating band: one block per run of buckets with the burner on for the radiators
    lv_obj_clean(s_heat);
    for (int i = 0, from = -1; i <= npts; i++) {
        bool on = i < npts && heat[i] > 0;
        if (on && from < 0) {
            from = i;
        } else if (!on && from >= 0) {
            lv_obj_t *r = lv_obj_create(s_heat);
            lv_obj_remove_style_all(r);
            lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(r, ui_c(UI_HEAT), 0);
            int x0 = from * CH_W / npts, x1 = i * CH_W / npts;
            lv_obj_set_pos(r, x0, 0);
            lv_obj_set_size(r, x1 > x0 ? x1 - x0 : 1, 22);
            from = -1;
        }
    }

    // Time labels. With the clock set they sit on round times (TICK_S); without it, 5 evenly spaced "ago" labels.
    int nt = 5, x0 = 0, xw = CH_W;
    if (!clock) {
        for (int i = 0; i < 5; i++) {
            int ago = (4 - i) * range_s / 4;
            if (ago == 0) {
                snprintf(s_xlbl[i], sizeof(s_xlbl[i]), "now");
            } else if (ago < 3600) {
                snprintf(s_xlbl[i], sizeof(s_xlbl[i]), "-%d min", ago / 60);
            } else {
                snprintf(s_xlbl[i], sizeof(s_xlbl[i]), "-%d h", ago / 3600);
            }
        }
    } else {
        const int step = TICK_S[s_rng];
        time_t start = now - range_s;
        struct tm lt;
        localtime_r(&start, &lt);
        int into = (lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec) % step;  // local time past the last round time
        time_t first = start + (into ? step - into : 0);
        nt = (int)((now - first) / step) + 1;
        nt = nt > X_TICKS ? X_TICKS : nt;
        x0 = (int)((first - start) * (long)CH_W / range_s);
        xw = (int)((long)(nt - 1) * step * CH_W / range_s);
        for (int i = 0; i < nt; i++) {
            time_t t = first + (time_t)i * step;
            localtime_r(&t, &lt);
            strftime(s_xlbl[i], sizeof(s_xlbl[i]), s_rng == R_WEEK ? "%a" : "%H:%M", &lt);
        }
    }
    for (int i = 0; i < X_TICKS; i++) {
        s_xtxt[i] = i < nt ? s_xlbl[i] : NULL;
        lv_obj_set_hidden(s_vgrid[i], i >= nt);
        if (i < nt) {
            lv_obj_set_x(s_vgrid[i], CH_X + x0 + (nt > 1 ? i * xw / (nt - 1) : 0));
        }
    }
    s_xtxt[nt] = NULL;
    if (nt < 2) {  // a scale needs two ticks
        s_xtxt[1] = "", s_xtxt[2] = NULL, nt = 2, xw = 2;
    }
    lv_obj_set_x(s_xscale, CH_X + x0);
    lv_obj_set_width(s_xscale, xw > 0 ? xw : 2);
    lv_scale_set_range(s_xscale, 0, nt - 1);
    lv_scale_set_total_tick_count(s_xscale, nt);
    lv_scale_set_text_src(s_xscale, s_xtxt);

    char rs[24] = "--", ts[24] = "--", fs[24] = "--";
    if (!isnan(cur_room)) {
        snprintf(rs, sizeof(rs), "%.1f °C", cur_room);
    }
    if (!isnan(cur_target)) {
        snprintf(ts, sizeof(ts), "%.1f °C", cur_target);
    }
    if (!isnan(cur_flow)) {
        snprintf(fs, sizeof(fs), "%.0f °C", cur_flow);
    }
    char water[48];
    if (s_water_off) {
        snprintf(water, sizeof(water), "#607D8B Water hidden#");
    } else {
        snprintf(water, sizeof(water), "#E57373 Water# %s", fs);
    }
    ui_label_fmt(s_chart_info, "#4FC3F7 Room# %s   #FFB74D Target# %s   %s\n#FF7043 Heating# on %d %% of the time", rs,
                 ts, water, heat_n ? 100 * heat_on / heat_n : 0);
}

static void water_cb(lv_event_t *e)
{
    s_water_off = !s_water_off;
    update_chart();
}

static void range_cb(lv_event_t *e)
{
    uint32_t id = lv_buttonmatrix_get_selected_button(s_range);
    if (id < R_COUNT) {
        s_rng = (int)id;
        update_chart();
    }
}

static lv_obj_t *axis(lv_obj_t *p, lv_scale_mode_t mode, int x, int y, int w, int h)
{
    lv_obj_t *s = lv_scale_create(p);
    lv_scale_set_mode(s, mode);
    lv_obj_set_pos(s, x, y);
    lv_obj_set_size(s, w, h);
    lv_scale_set_label_show(s, true);
    lv_obj_set_style_text_font(s, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s, ui_c(UI_MUTED), 0);
    lv_obj_set_style_line_color(s, ui_c(UI_DOT_OFF), LV_PART_MAIN);
    lv_obj_set_style_line_color(s, ui_c(UI_DOT_OFF), LV_PART_ITEMS);
    lv_obj_set_style_line_color(s, ui_c(UI_DOT_OFF), LV_PART_INDICATOR);
    lv_obj_set_style_length(s, 4, LV_PART_ITEMS);
    lv_obj_set_style_length(s, 8, LV_PART_INDICATOR);
    return s;
}

static void build_chart(lv_obj_t *p)
{
    lv_obj_t *t = lv_label_create(p);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_label_set_text(t, "History");
    lv_obj_set_pos(t, 20, 24);

    static const char *map[] = {"5 min", "1 h", "6 h", "24 h", "Week", ""};
    s_range = lv_buttonmatrix_create(p);
    lv_buttonmatrix_set_map(s_range, map);
    lv_buttonmatrix_set_button_ctrl_all(s_range, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(s_range, true);
    lv_buttonmatrix_set_button_ctrl(s_range, s_rng, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_pos(s_range, 20, 84);
    lv_obj_set_size(s_range, 440, 56);
    lv_obj_set_style_pad_all(s_range, 4, 0);
    lv_obj_set_style_pad_gap(s_range, 4, 0);
    lv_obj_add_event_cb(s_range, range_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Plot: room/target on the left scale, water on the right scale, heating as a band below
    const int cx = CH_X, cy = CH_Y, cw = CH_W, ch = CH_H;
    for (int i = 0; i < X_TICKS; i++) {  // vertical grid lines at the time labels, behind the (transparent) chart
        s_vgrid[i] = lv_obj_create(p);
        lv_obj_remove_style_all(s_vgrid[i]);
        lv_obj_set_size(s_vgrid[i], 1, ch);
        lv_obj_set_pos(s_vgrid[i], cx, cy);
        lv_obj_set_style_bg_opa(s_vgrid[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_vgrid[i], ui_c(UI_SURFACE), 0);
    }
    s_chart = lv_chart_create(p);
    lv_obj_set_pos(s_chart, cx, cy);
    lv_obj_set_size(s_chart, cw, ch);
    lv_obj_set_style_pad_all(s_chart, 0, 0);
    lv_obj_set_style_radius(s_chart, 0, 0);
    lv_obj_set_style_border_width(s_chart, 1, 0);
    lv_obj_set_style_border_color(s_chart, ui_c(UI_DOT_OFF), 0);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_line_color(s_chart, ui_c(UI_SURFACE), LV_PART_MAIN);  // grid
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(s_chart, 5, 0);
    lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR);  // no point markers
    lv_obj_set_style_line_width(s_chart, 3, LV_PART_ITEMS);
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_SECONDARY_Y, 200, 700);  // water 20-70 °C
    s_ser_flow = lv_chart_add_series(s_chart, ui_c(0xE57373), LV_CHART_AXIS_SECONDARY_Y);
    s_ser_target = lv_chart_add_series(s_chart, ui_c(UI_WARN), LV_CHART_AXIS_PRIMARY_Y);
    s_ser_room = lv_chart_add_series(s_chart, ui_c(UI_VALUE), LV_CHART_AXIS_PRIMARY_Y);

    s_heat = lv_obj_create(p);
    lv_obj_remove_style_all(s_heat);
    lv_obj_set_pos(s_heat, cx, cy + ch + 4);
    lv_obj_set_size(s_heat, cw, 22);
    lv_obj_set_style_bg_opa(s_heat, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_heat, ui_c(UI_SURFACE_ALT), 0);
    lv_obj_set_scrollable(s_heat, false);
    lv_obj_t *hl = lv_label_create(p);  // names the band
    lv_obj_set_style_text_color(hl, ui_c(UI_HEAT), 0);
    lv_label_set_text(hl, UI_ICON_FIRE);
    lv_obj_set_style_text_font(hl, &font_icons_20, 0);
    lv_obj_set_pos(hl, cx - 26, cy + ch + 4);

    s_yscale = axis(p, LV_SCALE_MODE_VERTICAL_LEFT, 8, cy, cx - 10, ch);
    s_wscale = axis(p, LV_SCALE_MODE_VERTICAL_RIGHT, cx + cw + 2, cy, 480 - cx - cw - 4, ch);
    lv_scale_set_range(s_wscale, 20, 70);
    lv_scale_set_total_tick_count(s_wscale, 6);
    lv_scale_set_major_tick_every(s_wscale, 1);
    lv_obj_set_style_text_color(s_wscale, ui_c(0xE57373), 0);
    s_xscale = axis(p, LV_SCALE_MODE_HORIZONTAL_BOTTOM, cx, cy + ch + 28, cw, 36);
    lv_scale_set_range(s_xscale, 0, 4);
    lv_scale_set_total_tick_count(s_xscale, 5);
    lv_scale_set_major_tick_every(s_xscale, 1);

    lv_obj_t *u = lv_label_create(p);
    lv_obj_set_style_text_font(u, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(u, ui_c(UI_MUTED), 0);
    lv_label_set_text(u, "°C");
    lv_obj_set_pos(u, 30, cy - 28);
    s_wlbl = lv_label_create(p);
    lv_obj_set_style_text_font(s_wlbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_wlbl, ui_c(0xE57373), 0);
    lv_label_set_text(s_wlbl, "water");
    lv_obj_set_pos(s_wlbl, cx + cw - 12, cy - 28);

    // Legend with the current values; a tap hides or shows the water line
    s_chart_info = lv_label_create(p);
    lv_label_set_recolor(s_chart_info, true);
    lv_obj_set_width(s_chart_info, 440);
    lv_obj_set_style_text_align(s_chart_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_chart_info, 20, cy + ch + 70);
    lv_obj_set_clickable(s_chart_info, true);
    lv_obj_set_ext_click_area(s_chart_info, 8);
    lv_obj_add_event_cb(s_chart_info, water_cb, LV_EVENT_CLICKED, NULL);
    s_cbuf = heap_caps_malloc(sizeof(thermo_fine_t) * THERMO_FINE_LEN, MALLOC_CAP_SPIRAM);
}

/* --------------------------------------------------------------- general */

static int active_page(void)
{
    lv_obj_t *act = lv_tileview_get_tile_active(s_tv);
    return act ? lv_obj_get_index(act) : 0;
}

static void update_dots(void)
{
    int page = active_page();
    for (int i = 0; i < 5; i++) {
        bool on = page == i;
        lv_obj_set_style_bg_color(s_dots[i], ui_c(on ? UI_TEXT : UI_DOT_OFF), 0);
    }
}

static void update_page(int page)
{
    switch (page) {
    case 1: update_chart(); break;
    case 2: ui_pages_schedule_update(); break;
    case 3: ui_pages_boiler_update(); break;
    case 4: ui_pages_settings_update(); break;
    default: break;
    }
}

static void dot_cb(lv_event_t *e)
{
    app_thermo_show_page((int)(intptr_t)lv_event_get_user_data(e));
}

static void strip_cb(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP) {
        lv_indev_wait_release(lv_indev_active());
        app_thermo_show_page(0);
    }
}

static void tile_cb(lv_event_t *e)
{
    update_dots();
    update_page(active_page());
}

static void timer_cb(lv_timer_t *t)
{
    if (lv_screen_active() != s_scr) {
        return;
    }
    update_main();
    static int n;
    int page = active_page();
    ++n;
    // Chart: every 5 s on the 5 min / 1 h views, every minute otherwise
    if (page >= 2 || (page == 1 && n % (s_rng <= R_1H ? 5 : 60) == 0)) {
        update_page(page);
    }
}

static void screen_cb(lv_event_t *e)
{
    update_main();
}

int app_thermo_page(void)
{
    return active_page();
}

void app_thermo_show_page(int page)
{
    lv_obj_update_layout(s_tv);
    lv_tileview_set_tile_by_index(s_tv, page, 0, LV_ANIM_ON);
    update_dots();
    update_page(page);
}

lv_obj_t *app_thermo_create(void)
{
    lv_obj_t *content;
    s_scr = ui_plain_screen(&content);
    lv_obj_add_event_cb(s_scr, screen_cb, LV_EVENT_SCREEN_LOAD_START, NULL);

    s_tv = lv_tileview_create(content);
    lv_obj_set_size(s_tv, BOARD_LCD_H_RES, BOARD_LCD_V_RES - UI_STATUS_H);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_tv, tile_cb, LV_EVENT_VALUE_CHANGED, NULL);

    build_main(lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_RIGHT));
    lv_obj_t *tiles[4];
    for (int i = 0; i < 4; i++) {
        tiles[i] = lv_tileview_add_tile(s_tv, i + 1, 0, i < 3 ? LV_DIR_LEFT | LV_DIR_RIGHT : LV_DIR_LEFT);
    }
    build_chart(tiles[0]);
    ui_pages_init(s_scr);
    ui_pages_schedule(tiles[1]);
    ui_pages_boiler(tiles[2]);
    ui_pages_settings(tiles[3]);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *apps = ui_button(tiles[i], LV_SYMBOL_BARS " Apps", ui_c(0x37474F), apps_cb, NULL);
        lv_obj_set_size(apps, 120, 56);
        lv_obj_align(apps, LV_ALIGN_TOP_RIGHT, -16, 16);
    }

    // Bottom strip: page dots (tap = go to that page); swipe up from here = back to page 1
    lv_obj_t *strip = lv_obj_create(content);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, BOARD_LCD_H_RES, 56);
    lv_obj_set_pos(strip, 0, BOARD_LCD_V_RES - UI_STATUS_H - 56);
    lv_obj_set_clickable(strip, true);
    lv_obj_set_gesture_bubble(strip, false);
    lv_obj_set_scrollable(strip, false);
    lv_obj_add_event_cb(strip, strip_cb, LV_EVENT_GESTURE, NULL);
    for (int i = 0; i < 5; i++) {
        lv_obj_t *hit = lv_obj_create(strip);
        lv_obj_remove_style_all(hit);
        lv_obj_set_size(hit, 44, 56);
        lv_obj_set_pos(hit, 240 - 110 + i * 44, 0);
        lv_obj_set_clickable(hit, true);
        lv_obj_set_gesture_bubble(hit, true);
        lv_obj_add_event_cb(hit, dot_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_dots[i] = lv_obj_create(hit);
        lv_obj_remove_style_all(s_dots[i]);
        lv_obj_set_size(s_dots[i], 12, 12);
        lv_obj_set_style_radius(s_dots[i], 6, 0);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
        lv_obj_align(s_dots[i], LV_ALIGN_CENTER, 0, 4);
        lv_obj_set_clickable(s_dots[i], false);
    }
    update_dots();
    update_main();
    lv_timer_create(timer_cb, 1000, NULL);
    return s_scr;
}
