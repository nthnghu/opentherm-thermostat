// Thermostat app pages 3-5: boiler card, schedule, settings. Editors open as their own screens and return to
// the thermostat (or to the list they came from). All settings go through cfgall (the same tables as CFG/web).
#include "ui_pages.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "esp_timer.h"
#include "ui.h"
#include "ui_theme.h"
#include "thermostat.h"
#include "cfgall.h"
#include "settings.h"
#include "timekeep.h"

static lv_obj_t *s_thermo_scr;
static const char *DAYS[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
#define C_COMFORT 0xFF7043
#define C_ECO     0x1565C0

/* ------------------------------------------------------------- helpers */

static lv_obj_t *label(lv_obj_t *p, const char *txt, int x, int y, int w, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_recolor(l, true);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    lv_obj_set_style_text_color(l, ui_c(color), 0);
    if (font) {
        lv_obj_set_style_text_font(l, font, 0);
    }
    ui_label_text(l, txt);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *button(lv_obj_t *p, const char *txt, uint32_t color, lv_event_cb_t cb, void *user, int x, int y,
                        int w, int h)
{
    lv_obj_t *b = ui_button(p, txt, ui_c(color), cb, user);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    return b;
}

static void set_btn_text(lv_obj_t *b, const char *txt)
{
    lv_label_set_text(lv_obj_get_child(b, 0), txt);
}

static void back_cb(lv_event_t *e);

// An app-style screen whose header button runs cb (instead of going to the launcher).
static lv_obj_t *screen(const char *title, lv_obj_t **content, lv_event_cb_t cb)
{
    lv_obj_t *scr = ui_app_screen(title, content);
    lv_obj_t *back = lv_obj_get_child(lv_obj_get_child(scr, 0), 0);
    for (int i = (int)lv_obj_get_event_count(back) - 1; i >= 0; i--) {
        lv_obj_remove_event(back, (uint32_t)i);
    }
    lv_obj_add_event_cb(back, cb ? cb : back_cb, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_obj_get_child(back, 0), LV_SYMBOL_LEFT " Back");
    return scr;
}

static void back_cb(lv_event_t *e)
{
    lv_screen_load(s_thermo_scr);
}

// Friendly text for a setting value.
static void pretty(const ct_field_t *f, const char *v, char *out, size_t size)
{
    if (f->type == CT_BOOL) {
        snprintf(out, size, "%s", strcmp(v, "0") ? "On" : "Off");
    } else if (strcmp(f->name, "dhw_mode") == 0) {
        static const char *n[] = {"Always", "Windows", "Off"};
        int k = atoi(v);
        snprintf(out, size, "%s", k >= 0 && k <= 2 ? n[k] : v);
    } else if (strcmp(f->name, "loss_mode") == 0) {
        static const char *n[] = {"Keep last", "Heating off", "Fixed flow"};
        int k = atoi(v);
        snprintf(out, size, "%s", k >= 0 && k <= 2 ? n[k] : v);
    } else if (strcmp(f->name, "strategy") == 0) {
        snprintf(out, size, "%s", atoi(v) ? "PI" : "On/off");
    } else if (strcmp(f->name, "pasteur_day") == 0) {
        int k = atoi(v);
        snprintf(out, size, "%s", k >= 0 && k < 7 ? DAYS[k] : v);
    } else if (f->type == CT_FLOAT) {
        float x = strtof(v, NULL);
        snprintf(out, size, fabsf(x - roundf(x)) < 0.005f ? "%.0f" : (fabsf(x * 10 - roundf(x * 10)) < 0.05f ? "%.1f" : "%.2f"), x);
    } else {
        snprintf(out, size, "%s", v);
    }
}

/* ------------------------------------------------------- field editor */

static lv_obj_t *s_fe_scr, *s_fe_name, *s_fe_help, *s_fe_val, *s_fe_table;
static lv_obj_t *s_fe_return;
static int s_fe_idx = -1;
static char s_fe_cur[24];
static void (*s_fe_done)(void);

static void fe_show(void)
{
    const ct_field_t *f = cfgall_field(s_fe_idx, NULL);
    char p[32];
    pretty(f, s_fe_cur, p, sizeof(p));
    lv_label_set_text(s_fe_val, p);
}

static void fe_step_cb(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    const ct_field_t *f = cfgall_field(s_fe_idx, NULL);
    char n[24];
    ct_step_value(f, s_fe_cur, dir, n, sizeof(n));
    strlcpy(s_fe_cur, n, sizeof(s_fe_cur));
    fe_show();
}

static void fe_close(void)
{
    lv_screen_load(s_fe_return ? s_fe_return : s_thermo_scr);
    if (s_fe_done) {
        s_fe_done();
    }
}

static void fe_save_cb(lv_event_t *e)
{
    const ct_field_t *f = cfgall_field(s_fe_idx, NULL);
    if (!cfgall_set(f->name, s_fe_cur)) {
        lv_label_set_text(s_fe_val, "refused");
        return;
    }
    fe_close();
}

static void fe_cancel_cb(lv_event_t *e)
{
    fe_close();
}

static void open_field(int idx, lv_obj_t *ret, void (*done)(void))
{
    cfg_table_t t;
    const ct_field_t *f = cfgall_field(idx, &t);
    if (!f) {
        return;
    }
    if (!s_fe_scr) {
        lv_obj_t *c;
        s_fe_scr = screen("Setting", &c, fe_cancel_cb);
        s_fe_table = label(c, "", 20, 12, 440, UI_MUTED, NULL);
        s_fe_name = label(c, "", 20, 40, 440, UI_TEXT, &lv_font_montserrat_28);
        s_fe_help = label(c, "", 20, 86, 440, UI_MUTED, NULL);
        s_fe_val = label(c, "", 0, 230, 0, UI_VALUE, &lv_font_montserrat_48);
        lv_obj_set_width(s_fe_val, 480);
        lv_obj_set_style_text_align(s_fe_val, LV_TEXT_ALIGN_CENTER, 0);
        for (int k = 0; k < 2; k++) {
            lv_obj_t *b = button(c, k ? LV_SYMBOL_PLUS : LV_SYMBOL_MINUS, k ? 0xD84315 : 0x1565C0, fe_step_cb,
                                 (void *)(intptr_t)(k ? 1 : -1), k ? 300 : 20, 320, 160, 90);
            lv_obj_add_event_cb(b, fe_step_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)(k ? 1 : -1));
            lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &lv_font_montserrat_40, 0);
        }
        button(c, "Cancel", 0x37474F, fe_cancel_cb, NULL, 20, 560, 210, 72);
        button(c, LV_SYMBOL_OK " Save", 0x2E7D32, fe_save_cb, NULL, 250, 560, 210, 72);
    }
    s_fe_idx = idx;
    s_fe_return = ret;
    s_fe_done = done;
    cfgall_get(idx, s_fe_cur, sizeof(s_fe_cur));
    char range[64];
    if (f->type == CT_BOOL) {
        snprintf(range, sizeof(range), "%s  -  on / off", cfgall_table_title(t));
    } else if (f->type == CT_TIME || f->type == CT_TIME_OPT) {
        snprintf(range, sizeof(range), "%s  -  time of day", cfgall_table_title(t));
    } else {
        snprintf(range, sizeof(range), "%s  -  %g to %g", cfgall_table_title(t), f->min, f->max);
    }
    lv_label_set_text(s_fe_table, range);
    lv_label_set_text(s_fe_name, f->name);
    lv_label_set_text(s_fe_help, f->help);
    fe_show();
    lv_screen_load(s_fe_scr);
}

/* -------------------------------------------------------- boiler card */

#define BL_ROWS 12
static lv_obj_t *s_bl_val[BL_ROWS], *s_bl_alerts;

void ui_pages_boiler(lv_obj_t *p)
{
    static const char *keys[BL_ROWS] = {"Boiler",      "Burner",          "Flow / return", "Modulation",
                                        "Flow setpoint", "Water pressure", "Tank",          "Controller",
                                        "Starts",      "Burns (24 h)",    "Efficiency (24 h)", "Gas (estimate)"};
    label(p, "Boiler", 20, 16, 0, UI_TEXT, &lv_font_montserrat_28);
    for (int i = 0; i < BL_ROWS; i++) {
        label(p, keys[i], 20, 66 + i * 40, 170, UI_MUTED, NULL);
        s_bl_val[i] = label(p, "-", 190, 66 + i * 40, 280, UI_TEXT, NULL);
    }
    s_bl_alerts = label(p, "", 20, 66 + BL_ROWS * 40 + 10, 440, UI_OK, NULL);
}

void ui_pages_boiler_update(void)
{
    thermo_state_t st;
    thermo_get(&st);
    dl_kpi_t k1, k24;
    thermo_get_kpi(&k1, &k24);
    char b[96];
    snprintf(b, sizeof(b), "%s %s", st.boiler_sim ? "simulated," : "real,",
             st.boiler_online ? "#81C784 answering#" : "#E57373 not answering#");
    ui_label_text(s_bl_val[0], b);
    snprintf(b, sizeof(b), "%s", !st.flame ? "off" : (st.dhw_active ? "#FF7043 on - hot water#" : "#FF7043 on - heating#"));
    ui_label_text(s_bl_val[1], b);
    lv_label_set_text_fmt(s_bl_val[2], "%.1f / %.1f °C", st.flow_c, st.return_c);
    lv_label_set_text_fmt(s_bl_val[3], "%.0f %%", st.modulation > 100 ? 100 : st.modulation);
    lv_label_set_text_fmt(s_bl_val[4], "%s %.0f °C", st.demand ? "heat to" : "off,", st.flow_setpoint);
    if (st.press_valid) {
        lv_label_set_text_fmt(s_bl_val[5], "%.2f bar", st.pressure_bar);
    } else {
        lv_label_set_text(s_bl_val[5], "not reported");
    }
    if (st.tank_valid) {
        lv_label_set_text_fmt(s_bl_val[6], "%.1f °C, hot water %s", st.tank_c, sched_dhw_reason_name(st.dhw_reason));
    } else {
        lv_label_set_text(s_bl_val[6], "not reported");
    }
    lv_label_set_text_fmt(s_bl_val[7], "%s, asks %.1f °C", ctrl_state_name(st.ctrl_state), st.req_c);
    lv_label_set_text_fmt(s_bl_val[8], "%u last hour, %.1f/h (24 h)", k1.ch_starts, k24.ch_starts_h);
    if (k24.ch_burns) {
        lv_label_set_text_fmt(s_bl_val[9], "median %.0f min, %.0f %% short", k24.median_burn_min, k24.short_pct);
    } else {
        lv_label_set_text(s_bl_val[9], "none completed yet");
    }
    lv_label_set_text_fmt(s_bl_val[10], "%.0f %% condensing, mod %.0f %%", k24.cond_pct, k24.mean_mod);
    lv_label_set_text_fmt(s_bl_val[11], "%.1f kWh (24 h), %.1f since start", k24.gas_kwh, st.gas_kwh_total);
    if (st.alert_count) {
        char a[300];
        size_t n = (size_t)snprintf(a, sizeof(a), "#FFB74D " LV_SYMBOL_WARNING " Alerts:#");
        for (int i = 0; i < ALERT_COUNT && n < sizeof(a); i++) {
            if (st.alerts & (1u << i)) {
                n += (size_t)snprintf(a + n, sizeof(a) - n, "\n- %s", alert_title(i));
            }
        }
        ui_label_text(s_bl_alerts, a);
    } else {
        ui_label_text(s_bl_alerts, "#81C784 " LV_SYMBOL_OK " No alerts#");
    }
}

/* ----------------------------------------------------------- schedule */

static lv_obj_t *s_week[7], *s_prog_btn, *s_boost_btn, *s_away_btn, *s_dhw_btn, *s_dhwb_btn, *s_sched_info;
static sched_config_t s_week_cache;
static bool s_week_valid;

static void draw_day(int d, const sched_config_t *c)
{
    lv_obj_t *bar = s_week[d];
    lv_obj_clean(bar);
    lv_obj_set_style_bg_color(bar, ui_c(c->prog_on ? C_ECO : UI_SURFACE_ALT), 0);
    int w = lv_obj_get_width(bar);
    // Walk the day in 15-minute steps and draw Comfort spans
    int start = -1;
    for (int m = 0; m <= 1440; m += 15) {
        bool comfort = m < 1440 && sched_level_at(c, d, m) == SCHED_COMFORT;
        if (comfort && start < 0) {
            start = m;
        } else if (!comfort && start >= 0) {
            lv_obj_t *r = lv_obj_create(bar);
            lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_border_width(r, 0, 0);
            lv_obj_set_style_radius(r, 4, 0);
            lv_obj_set_style_bg_color(r, ui_c(c->prog_on ? C_COMFORT : UI_FAINT), 0);
            lv_obj_set_pos(r, start * w / 1440, 0);
            lv_obj_set_size(r, (m - start) * w / 1440, LV_PCT(100));
            start = -1;
        }
    }
}

static void day_editor_open(int d);

static void day_cb(lv_event_t *e)
{
    day_editor_open((int)(intptr_t)lv_event_get_user_data(e));
}

static void prog_cb(lv_event_t *e)
{
    open_field(cfgall_find("prog_on"), s_thermo_scr, NULL);
}

static void boost_cb(lv_event_t *e)
{
    thermo_state_t st;
    thermo_get(&st);
    thermo_boost(st.boost_left_s == 0);
}

static void away_cb(lv_event_t *e)
{
    thermo_state_t st;
    thermo_get(&st);
    thermo_away(!st.away, 0);
}

static void dhw_mode_cb(lv_event_t *e)
{
    sched_config_t c;
    thermo_get_sched_config(&c);
    c.dhw_mode = (uint8_t)((c.dhw_mode + 1) % 3);
    thermo_set_sched_config(&c);
}

static void dhw_boost_cb(lv_event_t *e)
{
    thermo_state_t st;
    thermo_get(&st);
    thermo_dhw_boost(st.dhw_boost_left_s == 0);
}

void ui_pages_schedule(lv_obj_t *p)
{
    label(p, "Schedule", 20, 16, 0, UI_TEXT, &lv_font_montserrat_28);
    static const char *hours[5] = {"0", "6", "12", "18", "24"};
    for (int h = 0; h < 5; h++) {
        lv_obj_t *l = label(p, hours[h], 0, 54, 0, UI_FAINT, NULL);
        lv_obj_update_layout(l);
        lv_obj_set_x(l, 76 + h * 96 - lv_obj_get_width(l) / 2);
    }
    for (int d = 0; d < 7; d++) {
        label(p, DAYS[d], 20, 82 + d * 44, 0, UI_MUTED, NULL);
        lv_obj_t *bar = lv_obj_create(p);
        lv_obj_set_pos(bar, 76, 76 + d * 44);
        lv_obj_set_size(bar, 384, 34);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 6, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(bar, day_cb, LV_EVENT_CLICKED, (void *)(intptr_t)d);
        s_week[d] = bar;
    }
    s_prog_btn = button(p, "", 0x37474F, prog_cb, NULL, 20, 392, 440, 52);
    s_boost_btn = button(p, "", 0xD84315, boost_cb, NULL, 20, 456, 212, 64);
    s_away_btn = button(p, "", 0x37474F, away_cb, NULL, 248, 456, 212, 64);
    label(p, "Hot water", 20, 534, 0, UI_MUTED, NULL);
    s_dhw_btn = button(p, "", 0x37474F, dhw_mode_cb, NULL, 20, 560, 212, 64);
    s_dhwb_btn = button(p, "", 0x00838F, dhw_boost_cb, NULL, 248, 560, 212, 64);
    s_sched_info = label(p, "", 20, 636, 440, UI_MUTED, NULL);
}

void ui_pages_schedule_update(void)
{
    sched_config_t c;
    thermo_get_sched_config(&c);
    if (!s_week_valid || memcmp(&c, &s_week_cache, sizeof(c)) != 0) {
        lv_obj_update_layout(s_week[0]);
        for (int d = 0; d < 7; d++) {
            draw_day(d, &c);
        }
        s_week_cache = c;
        s_week_valid = true;
    }
    thermo_state_t st;
    thermo_get(&st);
    char b[96];
    if (!c.prog_on) {
        snprintf(b, sizeof(b), "Program off: constant %.1f °C", st.setpoint);
    } else {
        snprintf(b, sizeof(b), "Program on: %.1f / Eco %.1f °C", st.setpoint, st.setpoint - c.eco_setback_k);
    }
    set_btn_text(s_prog_btn, b);
    if (st.boost_left_s) {
        snprintf(b, sizeof(b), "Stop boost\n%d min left", (st.boost_left_s + 59) / 60);
    } else {
        snprintf(b, sizeof(b), c.boost_on ? "Boost\n+%.1f °C, %u min" : "Boost\n(switched off)", c.boost_k, c.boost_min);
    }
    set_btn_text(s_boost_btn, b);
    set_btn_text(s_away_btn, st.away ? "Back home" : (c.away_on ? "Away" : "Away\n(switched off)"));
    static const char *modes[] = {"Always on", "Time windows", "Off"};
    snprintf(b, sizeof(b), "Mode: %s", modes[c.dhw_mode <= 2 ? c.dhw_mode : 0]);
    set_btn_text(s_dhw_btn, b);
    if (st.dhw_boost_left_s) {
        snprintf(b, sizeof(b), "Stop boost\n%d min left", (st.dhw_boost_left_s + 59) / 60);
    } else {
        snprintf(b, sizeof(b), "Hot water boost");
    }
    set_btn_text(s_dhwb_btn, b);
    char tank[24] = "tank not reported";
    if (st.tank_valid) {
        snprintf(tank, sizeof(tank), "tank %.1f °C", st.tank_c);
    }
    lv_label_set_text_fmt(s_sched_info, "Hot water %s (%s), %s.\nPasteurisation: %s%s.", st.dhw_enable ? "enabled" : "off",
                          sched_dhw_reason_name(st.dhw_reason), tank, c.pasteur_on ? "weekly, last " : "off",
                          c.pasteur_on ? sched_pasteur_name(st.pasteur_result) : "");
}

/* --------------------------------------------------------- day editor */

static lv_obj_t *s_de_scr, *s_de_title, *s_de_row[SCHED_POINTS], *s_de_time[SCHED_POINTS], *s_de_lvl[SCHED_POINTS];
static lv_obj_t *s_de_copy[7], *s_de_msg;
static int s_de_day;
static sched_point_t s_de_pts[SCHED_POINTS];

static int de_count(void)
{
    int n = 0;
    while (n < SCHED_POINTS && s_de_pts[n].min != SCHED_UNUSED) {
        n++;
    }
    return n;
}

static void de_show(void)
{
    int n = de_count();
    for (int i = 0; i < SCHED_POINTS; i++) {
        if (i >= n) {
            lv_obj_add_flag(s_de_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_de_row[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(s_de_time[i], "%02d:%02d", s_de_pts[i].min / 60, s_de_pts[i].min % 60);
        bool c = s_de_pts[i].level == SCHED_COMFORT;
        set_btn_text(s_de_lvl[i], c ? "Comfort" : "Eco");
        lv_obj_set_style_bg_color(s_de_lvl[i], ui_c(c ? C_COMFORT : C_ECO), 0);
    }
}

static void de_time_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    int i = abs(v) - 1, dir = v > 0 ? 15 : -15;
    int m = s_de_pts[i].min + dir;
    s_de_pts[i].min = (uint16_t)(m < 0 ? 0 : (m > 1425 ? 1425 : m));
    de_show();
}

static void de_lvl_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    s_de_pts[i].level = s_de_pts[i].level == SCHED_COMFORT ? SCHED_ECO : SCHED_COMFORT;
    de_show();
}

static void de_del_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    for (int k = i; k < SCHED_POINTS - 1; k++) {
        s_de_pts[k] = s_de_pts[k + 1];
    }
    s_de_pts[SCHED_POINTS - 1] = (sched_point_t){SCHED_UNUSED, 0};
    de_show();
}

static void de_add_cb(lv_event_t *e)
{
    int n = de_count();
    if (n >= SCHED_POINTS) {
        return;
    }
    int m = n ? s_de_pts[n - 1].min + 60 : 360;
    s_de_pts[n] = (sched_point_t){(uint16_t)(m > 1425 ? 1425 : m),
                                  n && s_de_pts[n - 1].level == SCHED_COMFORT ? SCHED_ECO : SCHED_COMFORT};
    de_show();
}

static void de_save_cb(lv_event_t *e)
{
    char text[100];
    size_t n = 0;
    text[0] = 0;
    for (int i = 0; i < de_count(); i++) {
        n += (size_t)snprintf(text + n, sizeof(text) - n, "%s%02d:%02d=%c", n ? " " : "", s_de_pts[i].min / 60,
                              s_de_pts[i].min % 60, s_de_pts[i].level == SCHED_COMFORT ? 'c' : 'e');
    }
    sched_config_t c;
    thermo_get_sched_config(&c);
    bool ok = sched_prog_parse(&c, s_de_day, text);
    for (int d = 0; ok && d < 7; d++) {
        if (d != s_de_day && lv_obj_has_state(s_de_copy[d], LV_STATE_CHECKED)) {
            ok = sched_prog_parse(&c, d, text);
        }
    }
    if (!ok) {
        lv_label_set_text(s_de_msg, "Could not save these times");
        return;
    }
    thermo_set_sched_config(&c);
    lv_screen_load(s_thermo_scr);
}

static void de_cancel_cb(lv_event_t *e)
{
    lv_screen_load(s_thermo_scr);
}

static void day_editor_open(int d)
{
    if (!s_de_scr) {
        lv_obj_t *c;
        s_de_scr = screen("Day program", &c, de_cancel_cb);
        s_de_title = label(c, "", 20, 10, 440, UI_TEXT, &lv_font_montserrat_28);
        for (int i = 0; i < SCHED_POINTS; i++) {
            lv_obj_t *r = lv_obj_create(c);
            lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(r, 0, 0);
            lv_obj_set_style_pad_all(r, 0, 0);
            lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(r, 0, 56 + i * 68);
            lv_obj_set_size(r, 480, 64);
            s_de_row[i] = r;
            button(r, LV_SYMBOL_MINUS, 0x37474F, de_time_cb, (void *)(intptr_t)(-(i + 1)), 16, 4, 64, 56);
            s_de_time[i] = label(r, "", 92, 14, 0, UI_TEXT, &lv_font_montserrat_28);
            button(r, LV_SYMBOL_PLUS, 0x37474F, de_time_cb, (void *)(intptr_t)(i + 1), 186, 4, 64, 56);
            s_de_lvl[i] = button(r, "", C_COMFORT, de_lvl_cb, (void *)(intptr_t)i, 262, 4, 136, 56);
            button(r, LV_SYMBOL_CLOSE, 0x5D4037, de_del_cb, (void *)(intptr_t)i, 408, 4, 56, 56);
        }
        button(c, LV_SYMBOL_PLUS " Add a switch time", 0x37474F, de_add_cb, NULL, 16, 468, 448, 52);
        label(c, "Also apply to:", 20, 532, 0, UI_MUTED, NULL);
        for (int k = 0; k < 7; k++) {
            lv_obj_t *b = button(c, DAYS[k], 0x37474F, NULL, NULL, 16 + k * 64, 556, 58, 48);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CHECKABLE);
            lv_obj_set_style_bg_color(b, ui_c(0x2E7D32), LV_STATE_CHECKED);
            s_de_copy[k] = b;
        }
        s_de_msg = label(c, "", 20, 612, 440, UI_WARN, NULL);
        button(c, "Cancel", 0x37474F, de_cancel_cb, NULL, 16, 628, 214, 64);
        button(c, LV_SYMBOL_OK " Save", 0x2E7D32, de_save_cb, NULL, 250, 628, 214, 64);
    }
    sched_config_t c;
    thermo_get_sched_config(&c);
    s_de_day = d;
    memcpy(s_de_pts, c.prog[d], sizeof(s_de_pts));
    static const char *full[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    lv_label_set_text_fmt(s_de_title, "%s: switch times", full[d]);
    for (int k = 0; k < 7; k++) {
        lv_obj_remove_state(s_de_copy[k], LV_STATE_CHECKED);
        if (k == d) {
            lv_obj_add_state(s_de_copy[k], LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(s_de_copy[k], LV_STATE_DISABLED);
        }
    }
    lv_label_set_text(s_de_msg, "");
    de_show();
    lv_screen_load(s_de_scr);
}

/* ---------------------------------------------------- advanced list */

static lv_obj_t *s_adv_scr, **s_adv_val;
static int s_adv_n;

static void adv_refresh(void)
{
    char v[24], p[32];
    for (int i = 0; i < s_adv_n; i++) {
        cfgall_get(i, v, sizeof(v));
        pretty(cfgall_field(i, NULL), v, p, sizeof(p));
        lv_label_set_text(s_adv_val[i], p);
    }
}

static void adv_row_cb(lv_event_t *e)
{
    open_field((int)(intptr_t)lv_event_get_user_data(e), s_adv_scr, adv_refresh);
}

static void advanced_open(void)
{
    if (!s_adv_scr) {
        lv_obj_t *c;
        s_adv_scr = screen("All settings", &c, NULL);
        lv_obj_t *list = lv_obj_create(c);
        lv_obj_set_size(list, 480, UI_CONTENT_H);
        lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(list, 0, 0);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(list, 6, 0);
        s_adv_n = cfgall_count();
        s_adv_val = calloc(s_adv_n, sizeof(lv_obj_t *));
        int last_t = -1;
        for (int i = 0; i < s_adv_n; i++) {
            cfg_table_t t;
            const ct_field_t *f = cfgall_field(i, &t);
            if ((int)t != last_t) {
                lv_obj_t *h = lv_label_create(list);
                lv_obj_set_style_text_color(h, ui_c(UI_VALUE), 0);
                lv_obj_set_style_pad_top(h, 10, 0);
                lv_label_set_text(h, cfgall_table_title(t));
                last_t = (int)t;
            }
            lv_obj_t *b = lv_button_create(list);
            lv_obj_set_size(b, 440, 48);
            lv_obj_set_style_bg_color(b, ui_c(UI_SURFACE), 0);
            lv_obj_set_style_radius(b, 8, 0);
            lv_obj_add_event_cb(b, adv_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *n = lv_label_create(b);
            lv_label_set_text(n, f->name);
            lv_obj_align(n, LV_ALIGN_LEFT_MID, 4, 0);
            s_adv_val[i] = lv_label_create(b);
            lv_obj_set_style_text_color(s_adv_val[i], ui_c(UI_VALUE), 0);
            lv_obj_align(s_adv_val[i], LV_ALIGN_RIGHT_MID, -4, 0);
        }
    }
    adv_refresh();
    lv_screen_load(s_adv_scr);
}

/* -------------------------------------------------------------- service */

static lv_obj_t *s_sv_scr, *s_sv_pin, *s_sv_pin_lbl, *s_sv_pin_msg, *s_sv_main, *s_sv_status, *s_sv_mode_btn;
static lv_obj_t *s_sv_rows[SVC_COUNT], *s_sv_detail, *s_sv_d_title, *s_sv_d_help, *s_sv_d_res, *s_sv_d_start,
    *s_sv_d_apply, *s_sv_d_msg;
static char s_pin[8];
static int s_pin_fails;
static int64_t s_pin_locked_until, s_unlocked_at;
static int s_sv_sel = -1;
static lv_timer_t *s_sv_timer;

static void sv_update(void);

static void pin_show(void)
{
    char dots[16] = "";
    for (size_t i = 0; i < strlen(s_pin); i++) {
        strcat(dots, "* ");
    }
    lv_label_set_text(s_sv_pin_lbl, s_pin[0] ? dots : "Enter the service PIN");
}

static void pin_key_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    int64_t now = esp_timer_get_time();
    if (now < s_pin_locked_until) {
        lv_label_set_text(s_sv_pin_msg, "Too many wrong PINs: wait 5 minutes");
        return;
    }
    size_t n = strlen(s_pin);
    if (k >= 0 && k <= 9 && n < 6) {
        s_pin[n] = (char)('0' + k);
        s_pin[n + 1] = 0;
    } else if (k == 10 && n) {
        s_pin[n - 1] = 0;
    } else if (k == 11) {
        if (settings_check_svc_pin(s_pin)) {
            s_pin_fails = 0;
            s_unlocked_at = now;
            lv_obj_add_flag(s_sv_pin, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_sv_main, LV_OBJ_FLAG_HIDDEN);
            sv_update();
        } else {
            lv_label_set_text(s_sv_pin_msg, "Wrong PIN");
            if (++s_pin_fails >= 5) {
                s_pin_fails = 0;
                s_pin_locked_until = now + 300LL * 1000000;
            }
        }
        s_pin[0] = 0;
    }
    pin_show();
}

static void sv_mode_cb(lv_event_t *e)
{
    thermo_state_t st;
    thermo_get(&st);
    thermo_svc_enter(!st.svc_active);
}

static void sv_detail_show(void)
{
    svc_record_t *rec = malloc(sizeof(svc_record_t));
    if (!rec) {
        return;
    }
    thermo_svc_get(rec, NULL, 0);
    const svc_result_t *r = &rec->r[s_sv_sel];
    lv_label_set_text(s_sv_d_title, svc_routine_title(s_sv_sel));
    lv_label_set_text(s_sv_d_help, svc_routine_help(s_sv_sel));
    lv_label_set_text_fmt(s_sv_d_res, "Last result: %s%s\n%s", svc_rstate_name(r->state), r->sim ? " (simulated)" : "",
                          r->text);
    svc_apply_t a;
    if (svc_recommend(rec, s_sv_sel, &a)) {
        lv_obj_remove_flag(s_sv_d_apply, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_sv_d_apply, LV_OBJ_FLAG_HIDDEN);
    }
    free(rec);
}

static void sv_row_cb(lv_event_t *e)
{
    s_sv_sel = (int)(intptr_t)lv_event_get_user_data(e);
    lv_label_set_text(s_sv_d_msg, "");
    sv_detail_show();
    lv_obj_remove_flag(s_sv_detail, LV_OBJ_FLAG_HIDDEN);
}

static void sv_close_cb(lv_event_t *e)
{
    lv_obj_add_flag(s_sv_detail, LV_OBJ_FLAG_HIDDEN);
}

static void sv_start(bool confirmed)
{
    char why[128];
    bool ok = thermo_svc_start((svc_routine_t)s_sv_sel, confirmed, why, sizeof(why));
    lv_label_set_text(s_sv_d_msg, ok ? "Started" : why);
}

static void sv_confirm_cb(lv_event_t *e)
{
    lv_obj_t *box = lv_event_get_user_data(e);
    sv_start(true);
    lv_msgbox_close(box);
}

static void sv_box_close_cb(lv_event_t *e)
{
    lv_msgbox_close(lv_event_get_user_data(e));
}

static void sv_start_cb(lv_event_t *e)
{
    thermo_state_t st;
    thermo_get(&st);
    if (st.svc_running) {
        thermo_svc_cancel();
        lv_label_set_text(s_sv_d_msg, "Cancelled");
        return;
    }
    if (s_sv_sel == SVC_COMM_LOSS) {
        lv_obj_t *box = lv_msgbox_create(NULL);
        lv_msgbox_add_title(box, "Comm-loss test");
        lv_msgbox_add_text(box, "The thermostat goes silent. The boiler may then refuse heating until it is "
                                "power-cycled. Start only if you can get to the boiler.");
        lv_obj_add_event_cb(lv_msgbox_add_footer_button(box, "Start"), sv_confirm_cb, LV_EVENT_CLICKED, box);
        lv_obj_add_event_cb(lv_msgbox_add_footer_button(box, "Cancel"), sv_box_close_cb, LV_EVENT_CLICKED, box);
        return;
    }
    sv_start(false);
}

static void sv_apply_cb(lv_event_t *e)
{
    char msg[128];
    thermo_svc_apply((svc_routine_t)s_sv_sel, msg, sizeof(msg));
    lv_label_set_text(s_sv_d_msg, msg);
}

static void sv_update(void)
{
    if (!s_sv_scr || lv_obj_has_flag(s_sv_main, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    thermo_state_t st;
    thermo_get(&st);
    if (!st.svc_active) {
        ui_label_text(s_sv_status, "Service mode is off. Routines take over the boiler while they run.");
    } else if (st.svc_running) {
        char b[160];
        snprintf(b, sizeof(b), "#FFB74D Running: %s# (%d s)\n%s", svc_routine_title(st.svc_routine), st.svc_elapsed_s,
                 st.svc_step);
        ui_label_text(s_sv_status, b);
    } else {
        char b[96];
        snprintf(b, sizeof(b), "Service mode on (ends in %d min without activity). Pick a routine.", st.svc_idle_left_s / 60);
        ui_label_text(s_sv_status, b);
    }
    set_btn_text(s_sv_mode_btn, st.svc_active ? "Leave service mode" : "Enter service mode");
    svc_record_t *rec = malloc(sizeof(svc_record_t));
    if (rec) {
        thermo_svc_get(rec, NULL, 0);
        for (int i = 0; i < SVC_COUNT; i++) {
            lv_label_set_text_fmt(lv_obj_get_child(s_sv_rows[i], 1), "%s%s", svc_rstate_name(rec->r[i].state),
                                  rec->r[i].sim ? " (sim)" : "");
        }
        free(rec);
    }
    if (!lv_obj_has_flag(s_sv_detail, LV_OBJ_FLAG_HIDDEN)) {
        sv_detail_show();
        set_btn_text(s_sv_d_start, st.svc_running ? "Cancel routine" : "Start");
        if (!st.svc_active) {
            lv_obj_add_state(s_sv_d_start, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(s_sv_d_start, LV_STATE_DISABLED);
        }
    }
}

static void sv_timer_cb(lv_timer_t *t)
{
    if (lv_screen_active() != s_sv_scr) {
        return;
    }
    // Lock again 10 minutes after unlocking
    if (esp_timer_get_time() - s_unlocked_at > 600LL * 1000000 && lv_obj_has_flag(s_sv_pin, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(s_sv_pin, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_sv_main, LV_OBJ_FLAG_HIDDEN);
    }
    sv_update();
}

static void service_open(void)
{
    if (!s_sv_scr) {
        lv_obj_t *c;
        s_sv_scr = screen("Service", &c, NULL);
        // PIN pad
        s_sv_pin = lv_obj_create(c);
        lv_obj_set_size(s_sv_pin, 480, UI_CONTENT_H);
        lv_obj_set_style_bg_opa(s_sv_pin, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_sv_pin, 0, 0);
        lv_obj_set_style_pad_all(s_sv_pin, 0, 0);
        lv_obj_remove_flag(s_sv_pin, LV_OBJ_FLAG_SCROLLABLE);
        s_sv_pin_lbl = label(s_sv_pin, "", 20, 10, 440, UI_TEXT, &lv_font_montserrat_28);
        s_sv_pin_msg = label(s_sv_pin, "", 20, 54, 440, UI_WARN, NULL);
        static const char *keys[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK};
        static const int vals[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11};
        for (int i = 0; i < 12; i++) {
            lv_obj_t *b = button(s_sv_pin, keys[i], i == 11 ? 0x2E7D32 : 0x37474F, pin_key_cb, (void *)(intptr_t)vals[i],
                                 24 + (i % 3) * 148, 96 + (i / 3) * 104, 136, 92);
            lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &lv_font_montserrat_28, 0);
        }
        // Main
        s_sv_main = lv_obj_create(c);
        lv_obj_set_size(s_sv_main, 480, UI_CONTENT_H);
        lv_obj_set_style_bg_opa(s_sv_main, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_sv_main, 0, 0);
        lv_obj_set_style_pad_all(s_sv_main, 0, 0);
        lv_obj_remove_flag(s_sv_main, LV_OBJ_FLAG_SCROLLABLE);
        s_sv_status = label(s_sv_main, "", 20, 8, 440, UI_TEXT, NULL);
        s_sv_mode_btn = button(s_sv_main, "", 0x6A1B9A, sv_mode_cb, NULL, 20, 86, 440, 56);
        for (int i = 0; i < SVC_COUNT; i++) {
            lv_obj_t *b = lv_button_create(s_sv_main);
            lv_obj_set_pos(b, 20, 154 + i * 56);
            lv_obj_set_size(b, 440, 48);
            lv_obj_set_style_bg_color(b, ui_c(UI_SURFACE), 0);
            lv_obj_set_style_radius(b, 8, 0);
            lv_obj_add_event_cb(b, sv_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *n = lv_label_create(b);
            lv_label_set_text(n, svc_routine_title(i));
            lv_obj_align(n, LV_ALIGN_LEFT_MID, 4, 0);
            lv_obj_t *v = lv_label_create(b);
            lv_obj_set_style_text_color(v, ui_c(UI_VALUE), 0);
            lv_obj_align(v, LV_ALIGN_RIGHT_MID, -4, 0);
            s_sv_rows[i] = b;
        }
        // Routine detail
        s_sv_detail = ui_overlay(c);
        s_sv_d_title = label(s_sv_detail, "", 20, 10, 440, UI_TEXT, &lv_font_montserrat_28);
        s_sv_d_help = label(s_sv_detail, "", 20, 54, 440, UI_MUTED, NULL);
        s_sv_d_res = label(s_sv_detail, "", 20, 220, 440, UI_TEXT, NULL);
        s_sv_d_msg = label(s_sv_detail, "", 20, 440, 440, UI_WARN, NULL);
        s_sv_d_start = button(s_sv_detail, "Start", 0xD84315, sv_start_cb, NULL, 20, 500, 212, 64);
        s_sv_d_apply = button(s_sv_detail, "Apply", 0x2E7D32, sv_apply_cb, NULL, 248, 500, 212, 64);
        button(s_sv_detail, "Close", 0x37474F, sv_close_cb, NULL, 20, 584, 440, 64);
        s_sv_timer = lv_timer_create(sv_timer_cb, 1000, NULL);
    }
    s_pin[0] = 0;
    lv_obj_add_flag(s_sv_detail, LV_OBJ_FLAG_HIDDEN);
    bool unlocked = esp_timer_get_time() - s_unlocked_at < 600LL * 1000000 && s_unlocked_at;
    if (!settings_svc_pin_configured()) {
        lv_obj_remove_flag(s_sv_pin, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_sv_main, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_sv_pin_lbl, "Locked");
        lv_label_set_text(s_sv_pin_msg, "Set a service PIN on the web page (Service section) first.");
    } else if (unlocked) {
        lv_obj_add_flag(s_sv_pin, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_sv_main, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_sv_pin, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_sv_main, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_sv_pin_msg, "");
        pin_show();
    }
    sv_update();
    lv_screen_load(s_sv_scr);
}

/* ----------------------------------------------------------- settings */

static const char *EVERYDAY[] = {"prog_on", "eco_setback_k", "summer_on", "dhw_mode", "loss_mode", "frost_on",
                                 "frost_room_c", "ufh_on"};
#define N_EVERYDAY ((int)(sizeof(EVERYDAY) / sizeof(EVERYDAY[0])))
static lv_obj_t *s_ev_val[N_EVERYDAY];

static void ev_cb(lv_event_t *e)
{
    open_field(cfgall_find(EVERYDAY[(int)(intptr_t)lv_event_get_user_data(e)]), s_thermo_scr, NULL);
}

static void adv_cb(lv_event_t *e)
{
    advanced_open();
}

static void svc_cb(lv_event_t *e)
{
    service_open();
}

void ui_pages_settings(lv_obj_t *p)
{
    label(p, "Settings", 20, 16, 0, UI_TEXT, &lv_font_montserrat_28);
    static const char *titles[N_EVERYDAY] = {"Weekly program",     "Eco setback (K)",   "Summer mode (auto)",
                                             "Hot water",          "Room sensor lost",  "Frost protection",
                                             "Frost limit (°C)",   "Underfloor floor"};
    for (int i = 0; i < N_EVERYDAY; i++) {
        lv_obj_t *b = lv_button_create(p);
        lv_obj_set_pos(b, 20, 64 + i * 58);
        lv_obj_set_size(b, 440, 50);
        lv_obj_set_style_bg_color(b, ui_c(UI_SURFACE), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_add_event_cb(b, ev_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *n = lv_label_create(b);
        lv_label_set_text(n, titles[i]);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 4, 0);
        s_ev_val[i] = lv_label_create(b);
        lv_obj_set_style_text_color(s_ev_val[i], ui_c(UI_VALUE), 0);
        lv_obj_align(s_ev_val[i], LV_ALIGN_RIGHT_MID, -4, 0);
    }
    button(p, LV_SYMBOL_LIST " All settings", 0x37474F, adv_cb, NULL, 20, 544, 212, 64);
    button(p, LV_SYMBOL_SETTINGS " Service", 0x6A1B9A, svc_cb, NULL, 248, 544, 212, 64);
    label(p, "The same settings are on the web page, with export and import.", 20, 624, 440, UI_FAINT, NULL);
}

void ui_pages_settings_update(void)
{
    char v[24], pr[32];
    for (int i = 0; i < N_EVERYDAY; i++) {
        int k = cfgall_find(EVERYDAY[i]);
        if (k >= 0 && cfgall_get(k, v, sizeof(v))) {
            pretty(cfgall_field(k, NULL), v, pr, sizeof(pr));
            lv_label_set_text(s_ev_val[i], pr);
        }
    }
}

/* ---------------------------------------------------------------- misc */

void ui_pages_init(lv_obj_t *thermo_screen)
{
    s_thermo_scr = thermo_screen;
}

void ui_pages_debug(const char *what)
{
    if (strncmp(what, "edit ", 5) == 0) {
        open_field(cfgall_find(what + 5), s_thermo_scr, NULL);
    } else if (strcmp(what, "advanced") == 0) {
        advanced_open();
    } else if (strcmp(what, "service") == 0) {
        service_open();
    } else if (strncmp(what, "day ", 4) == 0) {
        day_editor_open(atoi(what + 4) % 7);
    }
}
