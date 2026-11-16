// Thermostat app: swipeable pages. Page 1 = room temperature, clock, setpoint; page 2 = 24 h chart;
// page 3 = boiler card, page 4 = schedule, page 5 = settings (ui_pages.c).
#include <stdio.h>
#include <time.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "board.h"
#include "ui.h"
#include "ui_theme.h"
#include "thermostat.h"
#include "timekeep.h"
#include "ui_pages.h"

#define CHART_POINTS 144  // 24 h at 10 min

static lv_obj_t *s_scr, *s_tv;
static lv_obj_t *s_time, *s_date, *s_room, *s_hum, *s_arc, *s_set, *s_mode_btn, *s_mode_lbl, *s_info;
static lv_obj_t *s_chart, *s_chart_info;
static lv_chart_series_t *s_ser_room, *s_ser_sp;
static lv_obj_t *s_dots[5];
static thermo_sample_t *s_hist;

/* ------------------------------------------------------------- main page */

static void minus_cb(lv_event_t *e)
{
    thermo_adjust_setpoint(-0.5f);
}

static void plus_cb(lv_event_t *e)
{
    thermo_adjust_setpoint(0.5f);
}

static void arc_cb(lv_event_t *e)
{
    thermo_set_setpoint(lv_arc_get_value(s_arc) / 2.0f);
}

static void mode_cb(lv_event_t *e)
{
    thermo_state_t st;
    thermo_get(&st);
    thermo_set_mode(st.mode == THERMO_MODE_HEAT ? THERMO_MODE_OFF : THERMO_MODE_HEAT);
}

static void info_cb(lv_event_t *e)
{
    app_thermo_show_page(2);  // the boiler card, with the alerts
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

    lv_label_set_text_fmt(s_set, "%.1f °C", st.setpoint);
    if (!lv_obj_has_state(s_arc, LV_STATE_PRESSED)) {
        lv_arc_set_value(s_arc, (int)lroundf(st.setpoint * 2));
    }
    bool heat = st.mode == THERMO_MODE_HEAT;
    lv_label_set_text(s_mode_lbl, heat ? LV_SYMBOL_POWER " Heat" : LV_SYMBOL_POWER " Off");
    lv_obj_set_style_bg_color(s_mode_btn, ui_c(heat ? 0xD84315 : 0x455A64), 0);
    lv_obj_set_style_arc_color(s_arc, ui_c(heat ? 0xFF7043 : 0x607D8B), LV_PART_INDICATOR);

    char boiler[64];
    if (st.boiler_online) {
        snprintf(boiler, sizeof(boiler), "%sflow %.0f °C, %.0f %%", st.boiler_sim ? "sim. boiler: " : "boiler: ",
                 st.flow_c, st.modulation);
    } else {
        snprintf(boiler, sizeof(boiler), "%s", st.boiler_sim ? "sim. boiler offline" : "no boiler connected");
    }
    // Schedule hint: "Eco 19.0 °C until 05:30  /  "
    char plan[64] = "";
    if (st.sched_src == SCHED_SRC_BOOST) {
        snprintf(plan, sizeof(plan), "Boost %.1f °C, %d min left  /  ", st.target_c, (st.boost_left_s + 59) / 60);
    } else if (st.sched_src == SCHED_SRC_AWAY) {
        if (st.away_until) {
            time_t u = (time_t)st.away_until;
            struct tm t;
            localtime_r(&u, &t);
            snprintf(plan, sizeof(plan), "Away %.0f °C until %02d.%02d %02d:%02d  /  ", st.target_c, t.tm_mon + 1,
                     t.tm_mday, t.tm_hour, t.tm_min);
        } else {
            snprintf(plan, sizeof(plan), "Away %.0f °C  /  ", st.target_c);
        }
    } else if (st.sched_src == SCHED_SRC_SUMMER) {
        snprintf(plan, sizeof(plan), "Summer: heating off  /  ");
    } else if (st.sched_src == SCHED_SRC_ECO || st.sched_src == SCHED_SRC_COMFORT) {
        const char *lvl = st.sched_src == SCHED_SRC_ECO ? "Eco" : "Comfort";
        if (st.next_change_min >= 0) {
            time_t u = time(NULL) + st.next_change_min * 60;
            struct tm t;
            localtime_r(&u, &t);
            snprintf(plan, sizeof(plan), "%s %.1f °C until %02d:%02d  /  ", lvl, st.target_c, t.tm_hour, t.tm_min);
        } else {
            snprintf(plan, sizeof(plan), "%s %.1f °C  /  ", lvl, st.target_c);
        }
    }
    ctrl_config_t cfg;
    thermo_get_ctrl_config(&cfg);
    char loss[64];
    if (cfg.loss_mode == CTRL_LOSS_FIXED) {
        snprintf(loss, sizeof(loss), "fallback heating %.0f °C, %u %%", cfg.loss_flow_c, cfg.loss_duty_pct);
    } else {
        snprintf(loss, sizeof(loss), "%s", cfg.loss_mode == CTRL_LOSS_HEAT_OFF ? "heating off" : "last setting kept");
    }
    if (st.svc_running) {
        lv_label_set_text_fmt(s_info, "#%06lX " LV_SYMBOL_SETTINGS " Service: %s# - %s", (unsigned long)ui_hex(UI_WARN),
                              svc_routine_title(st.svc_routine), st.svc_step);
    } else if (st.alert_count && !(st.alerts & (1u << ALERT_SENSOR))) {
        lv_label_set_text_fmt(s_info, "#%06lX " LV_SYMBOL_WARNING " %s#  /  %s%s", (unsigned long)ui_hex(UI_WARN),
                              st.alert_text, plan, boiler);
    } else if (!st.sensor_ok) {
        lv_label_set_text_fmt(s_info, "#%06lX " LV_SYMBOL_WARNING " Room sensor not answering - %s#",
                              (unsigned long)ui_hex(UI_ERR), loss);
    } else if (st.sensor_age_s >= 6) {
        lv_label_set_text_fmt(s_info, "#%06lX " LV_SYMBOL_WARNING " Room sensor not answering for %d s - %s in %d s#",
                              (unsigned long)ui_hex(UI_WARN), st.sensor_age_s, loss,
                              THERMO_SENSOR_STALE_S - st.sensor_age_s);
    } else if (st.demand) {
        const char *why = st.ctrl_state == CTRL_ST_HOLD      ? " (min-fire hold)"
                          : st.ctrl_state == CTRL_ST_DUTY_ON ? " (long burn, low load)"
                          : st.ctrl_state == CTRL_ST_FROST   ? " (frost protection)"
                                                             : "";
        lv_label_set_text_fmt(s_info, "#%06lX Heating# to %.0f °C%s  /  %s%s", (unsigned long)ui_hex(UI_HEAT),
                              st.flow_setpoint, why, plan, boiler);
    } else if (st.ctrl_state == CTRL_ST_DUTY_OFF && heat) {
        lv_label_set_text_fmt(s_info, "Pausing (low load: fewer, longer burns)  /  %s%s", plan, boiler);
    } else if (!heat && cfg.frost_on) {
        lv_label_set_text_fmt(s_info, "Idle (frost protection below %.0f °C)  /  %s", cfg.frost_room_c, boiler);
    } else {
        lv_label_set_text_fmt(s_info, "Idle  /  %s%s", heat ? plan : "", boiler);
    }
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
    lv_obj_set_size(s_arc, 380, 380);
    lv_obj_align(s_arc, LV_ALIGN_TOP_MID, 0, 104);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, (int)(THERMO_SP_MIN * 2), (int)(THERMO_SP_MAX * 2));
    lv_obj_set_style_arc_width(s_arc, 22, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 22, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s_arc, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(s_arc, arc_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_t *rl = lv_label_create(s_arc);
    lv_obj_set_style_text_color(rl, ui_c(0x90A4AE), 0);
    lv_label_set_text(rl, "Room");
    lv_obj_align(rl, LV_ALIGN_CENTER, 0, -86);
    s_room = lv_label_create(s_arc);
    lv_obj_set_style_text_font(s_room, &lv_font_montserrat_48, 0);
    lv_obj_set_style_transform_scale(s_room, 400, 0);  // 48 px font shown ~75 px tall
    lv_obj_set_style_transform_pivot_x(s_room, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_room, LV_PCT(50), 0);
    lv_obj_align(s_room, LV_ALIGN_CENTER, 0, -22);
    s_hum = lv_label_create(s_arc);
    lv_label_set_recolor(s_hum, true);
    lv_obj_set_style_text_color(s_hum, ui_c(0x4FC3F7), 0);
    lv_obj_align(s_hum, LV_ALIGN_CENTER, 0, 44);
    lv_obj_t *sl = lv_label_create(s_arc);
    lv_obj_set_style_text_color(sl, ui_c(0x90A4AE), 0);
    lv_label_set_text(sl, "Set to");
    lv_obj_align(sl, LV_ALIGN_CENTER, 0, 92);
    s_set = lv_label_create(s_arc);
    lv_obj_set_style_text_font(s_set, &lv_font_montserrat_28, 0);
    lv_obj_align(s_set, LV_ALIGN_CENTER, 0, 124);

    lv_obj_t *minus = ui_button(p, LV_SYMBOL_MINUS, ui_c(0x1565C0), minus_cb, NULL);
    lv_obj_set_size(minus, 120, 80);
    lv_obj_set_pos(minus, 20, 500);
    lv_obj_set_style_text_font(lv_obj_get_child(minus, 0), &lv_font_montserrat_40, 0);
    lv_obj_t *plus = ui_button(p, LV_SYMBOL_PLUS, ui_c(0xD84315), plus_cb, NULL);
    lv_obj_set_size(plus, 120, 80);
    lv_obj_set_pos(plus, 340, 500);
    lv_obj_set_style_text_font(lv_obj_get_child(plus, 0), &lv_font_montserrat_40, 0);
    s_mode_btn = ui_button(p, "", ui_c(0xD84315), mode_cb, NULL);
    lv_obj_set_size(s_mode_btn, 172, 80);
    lv_obj_set_pos(s_mode_btn, 154, 500);
    s_mode_lbl = lv_obj_get_child(s_mode_btn, 0);
    lv_obj_set_style_text_font(s_mode_lbl, &lv_font_montserrat_28, 0);

    s_info = lv_label_create(p);
    lv_label_set_recolor(s_info, true);
    lv_obj_set_width(s_info, 440);
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_info, 20, 600);
    lv_obj_add_flag(s_info, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_info, info_cb, LV_EVENT_CLICKED, NULL);
}

/* ------------------------------------------------------------ chart page */

static void update_chart(void)
{
    int n = thermo_history(s_hist, THERMO_HISTORY_LEN);
    float lo = 99, hi = -99, cur = NAN;
    lv_chart_set_all_values(s_chart, s_ser_room, LV_CHART_POINT_NONE);
    lv_chart_set_all_values(s_chart, s_ser_sp, LV_CHART_POINT_NONE);
    // Newest sample at the right edge; one chart point per 10 min.
    for (int k = 0; k < CHART_POINTS; k++) {
        int i = n - 1 - (CHART_POINTS - 1 - k) * 10;
        if (i < 0) {
            continue;
        }
        if (s_hist[i].room_c10 != INT16_MIN) {
            lv_chart_set_value_by_id(s_chart, s_ser_room, k, s_hist[i].room_c10);
            float v = s_hist[i].room_c10 / 10.0f;
            lo = fminf(lo, v);
            hi = fmaxf(hi, v);
            cur = v;
        }
        lv_chart_set_value_by_id(s_chart, s_ser_sp, k, s_hist[i].setpoint_c10);
        float sp = s_hist[i].setpoint_c10 / 10.0f;
        lo = fminf(lo, sp);
        hi = fmaxf(hi, sp);
    }
    if (lo > hi) {
        lo = 18;
        hi = 24;
    }
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, (int)floorf(lo * 10) - 5, (int)ceilf(hi * 10) + 5);
    lv_chart_refresh(s_chart);
    if (isnan(cur)) {
        lv_label_set_text_fmt(s_chart_info, "%d min recorded, no readings yet", n);
    } else {
        lv_label_set_text_fmt(s_chart_info, "Now %.1f °C  /  low %.1f  /  high %.1f\n%d min recorded since boot", cur, lo, hi,
                              n);
    }
}

static void build_chart(lv_obj_t *p)
{
    lv_obj_t *t = lv_label_create(p);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_label_set_text(t, "Last 24 hours");
    lv_obj_set_pos(t, 20, 16);
    lv_obj_t *lg = lv_label_create(p);
    lv_label_set_recolor(lg, true);
    lv_label_set_text(lg, "#4FC3F7 room#   #FFB74D setpoint#");
    lv_obj_set_pos(lg, 22, 58);

    s_chart = lv_chart_create(p);
    lv_obj_set_size(s_chart, 440, 460);
    lv_obj_set_pos(s_chart, 20, 96);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CHART_POINTS);
    lv_chart_set_div_line_count(s_chart, 5, 4);
    lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR);  // no point markers
    lv_obj_set_style_line_width(s_chart, 3, LV_PART_ITEMS);
    s_ser_room = lv_chart_add_series(s_chart, ui_c(0x4FC3F7), LV_CHART_AXIS_PRIMARY_Y);
    s_ser_sp = lv_chart_add_series(s_chart, ui_c(0xFFB74D), LV_CHART_AXIS_PRIMARY_Y);

    s_chart_info = lv_label_create(p);
    lv_obj_set_style_text_color(s_chart_info, ui_c(0x90A4AE), 0);
    lv_obj_set_pos(s_chart_info, 22, 572);
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
    case 2: ui_pages_boiler_update(); break;
    case 3: ui_pages_schedule_update(); break;
    case 4: ui_pages_settings_update(); break;
    default: break;
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
    if (page >= 2 || (++n % 60 == 0 && page == 1)) {
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
    lv_tileview_set_tile_by_index(s_tv, page, 0, LV_ANIM_OFF);
    update_dots();
    update_page(page);
}

lv_obj_t *app_thermo_create(void)
{
    s_hist = heap_caps_malloc(sizeof(thermo_sample_t) * THERMO_HISTORY_LEN, MALLOC_CAP_SPIRAM);
    lv_obj_t *content;
    s_scr = ui_plain_screen(&content);
    lv_obj_add_event_cb(s_scr, screen_cb, LV_EVENT_SCREEN_LOAD_START, NULL);

    s_tv = lv_tileview_create(content);
    lv_obj_set_size(s_tv, BOARD_LCD_H_RES, BOARD_LCD_V_RES - UI_STATUS_H);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_tv, tile_cb, LV_EVENT_VALUE_CHANGED, NULL);

    build_main(lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_RIGHT));
    build_chart(lv_tileview_add_tile(s_tv, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT));
    ui_pages_init(s_scr);
    ui_pages_boiler(lv_tileview_add_tile(s_tv, 2, 0, LV_DIR_LEFT | LV_DIR_RIGHT));
    ui_pages_schedule(lv_tileview_add_tile(s_tv, 3, 0, LV_DIR_LEFT | LV_DIR_RIGHT));
    ui_pages_settings(lv_tileview_add_tile(s_tv, 4, 0, LV_DIR_LEFT));

    // Page dots
    for (int i = 0; i < 5; i++) {
        s_dots[i] = lv_obj_create(content);
        lv_obj_remove_style_all(s_dots[i]);
        lv_obj_set_size(s_dots[i], 10, 10);
        lv_obj_set_style_radius(s_dots[i], 5, 0);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_pos(s_dots[i], 240 - 50 + i * 22, BOARD_LCD_V_RES - UI_STATUS_H - 24);
    }
    update_dots();
    update_main();
    lv_timer_create(timer_cb, 1000, NULL);
    return s_scr;
}
