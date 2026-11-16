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
#include "cfgtext.h"
#include "settings.h"
#include "timekeep.h"

static lv_obj_t *s_thermo_scr;
static const char *DAYS[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
#define C_COMFORT 0xFF7043
#define C_ECO     0x1565C0
#define C_FIXED_LO 0x9575CD  // fixed temperatures: violet, light (16 °C or less) to deep (23 °C+)
#define C_FIXED_HI 0x4527A0
#define C_FIXED   0x7E57C2

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

// Friendly text for a setting value ("2 °C", "On", "Time windows", "1 h 30 min").
static void pretty(const ct_field_t *f, const char *v, char *out, size_t size)
{
    cfgtext_format(f, v, out, size);
}

static const cfgtext_t *text_of(int idx)
{
    const ct_field_t *f = cfgall_field(idx, NULL);
    return f ? cfgtext_get(f->name) : NULL;
}

static const char *title_of(int idx)
{
    const cfgtext_t *t = text_of(idx);
    return t ? t->title : cfgall_field(idx, NULL)->name;
}

// False when the setting this one belongs to is off (or set to another choice): shown dimmed.
static bool in_use(const cfgtext_t *t)
{
    char pn[32], v[24];
    int pv;
    if (!cfgtext_parent(t, pn, sizeof(pn), &pv)) {
        return true;
    }
    int k = cfgall_find(pn);
    return k < 0 || !cfgall_get(k, v, sizeof(v)) || atoi(v) == pv;
}

// "Used only while 'Hot water' is 'Time windows'."
static void parent_note(const cfgtext_t *t, char *out, size_t size)
{
    char pn[32];
    int pv;
    out[0] = 0;
    if (!cfgtext_parent(t, pn, sizeof(pn), &pv)) {
        return;
    }
    const cfgtext_t *p = cfgtext_get(pn);
    int k = cfgall_find(pn);
    if (!p || k < 0) {
        return;
    }
    char raw[8], val[32];
    snprintf(raw, sizeof(raw), "%d", pv);
    pretty(cfgall_field(k, NULL), raw, val, sizeof(val));
    snprintf(out, size, "Used only while '%s' is '%s'.", p->title, val);
}

// A list row: title (left), value (right), one-line description below. Returns the value label.
static lv_obj_t *setting_row(lv_obj_t *parent, int idx, int x, int y, int w, lv_event_cb_t cb, void *user)
{
    const cfgtext_t *t = text_of(idx);
    lv_obj_t *b = lv_button_create(parent);
    if (y >= 0) {
        lv_obj_set_pos(b, x, y);
    } else {
        lv_obj_set_style_margin_left(b, x, 0);
    }
    lv_obj_set_size(b, w, 64);
    lv_obj_set_style_bg_color(b, ui_c(UI_SURFACE), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *n = lv_label_create(b);
    lv_obj_set_style_text_color(n, ui_c(UI_TEXT), 0);
    lv_label_set_text(n, title_of(idx));
    lv_obj_set_pos(n, 12, 8);
    lv_obj_t *d = lv_label_create(b);
    lv_obj_set_style_text_font(d, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(d, ui_c(UI_MUTED), 0);
    lv_obj_set_width(d, w - 24);
    lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);
    lv_label_set_text(d, t ? t->brief : "");
    lv_obj_set_pos(d, 12, 38);
    lv_obj_t *v = lv_label_create(b);
    lv_obj_set_style_text_color(v, ui_c(UI_VALUE), 0);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -12, 8);
    return v;
}

// Updates a row's value and dims it while the setting is not in use.
static void row_refresh(lv_obj_t *val, int idx)
{
    char v[24], p[32];
    if (!cfgall_get(idx, v, sizeof(v))) {
        return;
    }
    pretty(cfgall_field(idx, NULL), v, p, sizeof(p));
    lv_label_set_text(val, p);
    const cfgtext_t *t = text_of(idx);
    lv_obj_set_style_opa(lv_obj_get_parent(val), !t || in_use(t) ? LV_OPA_COVER : LV_OPA_50, 0);
}

/* ------------------------------------------------------- field editor */

static lv_obj_t *s_fe_scr, *s_fe_group, *s_fe_name, *s_fe_help, *s_fe_val, *s_fe_desc, *s_fe_info, *s_fe_btn[2];
static lv_obj_t *s_fe_return;
static int s_fe_idx = -1;
static char s_fe_cur[24];
static void (*s_fe_done)(void);

static void fe_show(void)
{
    const ct_field_t *f = cfgall_field(s_fe_idx, NULL);
    const cfgtext_t *t = cfgtext_get(f->name);
    char p[32];
    pretty(f, s_fe_cur, p, sizeof(p));
    lv_label_set_text(s_fe_val, p);
    int k = atoi(s_fe_cur);
    lv_label_set_text(s_fe_desc, t && t->opts && k >= 0 && k < t->nopts ? t->opts[k].desc : "");
    if (f->type == CT_BOOL) {
        bool on = k != 0;
        lv_obj_set_style_bg_color(s_fe_btn[0], ui_c(on ? 0x37474F : 0x1565C0), 0);
        lv_obj_set_style_bg_color(s_fe_btn[1], ui_c(on ? 0x2E7D32 : 0x37474F), 0);
    } else {
        lv_obj_set_style_bg_color(s_fe_btn[0], ui_c(0x1565C0), 0);
        lv_obj_set_style_bg_color(s_fe_btn[1], ui_c(0xD84315), 0);
    }
}

static void fe_step_cb(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    const ct_field_t *f = cfgall_field(s_fe_idx, NULL);
    if (f->type == CT_BOOL) {
        strlcpy(s_fe_cur, dir > 0 ? "1" : "0", sizeof(s_fe_cur));
    } else {
        char n[24];
        ct_step_value(f, s_fe_cur, dir, n, sizeof(n));
        strlcpy(s_fe_cur, n, sizeof(s_fe_cur));
    }
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
        lv_label_set_text(s_fe_desc, "Not accepted, please try another value");
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
    cfg_table_t tb;
    const ct_field_t *f = cfgall_field(idx, &tb);
    if (!f) {
        return;
    }
    const cfgtext_t *t = cfgtext_get(f->name);
    if (!s_fe_scr) {
        lv_obj_t *c;
        s_fe_scr = screen("Setting", &c, fe_cancel_cb);
        s_fe_group = label(c, "", 20, 8, 440, UI_MUTED, &lv_font_montserrat_14);
        s_fe_name = label(c, "", 20, 28, 440, UI_TEXT, &lv_font_montserrat_28);
        s_fe_help = label(c, "", 20, 72, 440, UI_CARD_TEXT, NULL);
        s_fe_val = label(c, "", 0, 290, 0, UI_VALUE, &lv_font_montserrat_40);
        lv_obj_set_width(s_fe_val, 480);
        lv_obj_set_style_text_align(s_fe_val, LV_TEXT_ALIGN_CENTER, 0);
        s_fe_desc = label(c, "", 20, 342, 440, UI_MUTED, NULL);
        lv_obj_set_style_text_align(s_fe_desc, LV_TEXT_ALIGN_CENTER, 0);
        for (int k = 0; k < 2; k++) {
            s_fe_btn[k] = button(c, "", k ? 0xD84315 : 0x1565C0, fe_step_cb, (void *)(intptr_t)(k ? 1 : -1),
                                 k ? 300 : 20, 410, 160, 84);
            lv_obj_add_event_cb(s_fe_btn[k], fe_step_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)(k ? 1 : -1));
        }
        s_fe_info = label(c, "", 20, 510, 440, UI_FAINT, NULL);
        lv_obj_set_style_text_align(s_fe_info, LV_TEXT_ALIGN_CENTER, 0);
        button(c, "Cancel", 0x37474F, fe_cancel_cb, NULL, 20, 590, 210, 72);
        button(c, LV_SYMBOL_OK " Save", 0x2E7D32, fe_save_cb, NULL, 250, 590, 210, 72);
    }
    s_fe_idx = idx;
    s_fe_return = ret;
    s_fe_done = done;
    cfgall_get(idx, s_fe_cur, sizeof(s_fe_cur));

    lv_label_set_text(s_fe_group, t ? cfgtext_group_title(t->group) : cfgall_table_title(tb));
    lv_label_set_text(s_fe_name, t ? t->title : f->name);
    char help[420], note[96];
    parent_note(t, note, sizeof(note));
    snprintf(help, sizeof(help), "%s%s%s", t ? t->help : f->help, note[0] ? "\n\n" : "", note);
    lv_label_set_text(s_fe_help, help);

    // Buttons: Off / On, previous / next choice, or minus / plus
    const lv_font_t *big = &lv_font_montserrat_40;
    if (f->type == CT_BOOL) {
        set_btn_text(s_fe_btn[0], "Off");
        set_btn_text(s_fe_btn[1], "On");
        big = &lv_font_montserrat_28;
    } else if (t && t->opts) {
        set_btn_text(s_fe_btn[0], LV_SYMBOL_LEFT);
        set_btn_text(s_fe_btn[1], LV_SYMBOL_RIGHT);
    } else {
        set_btn_text(s_fe_btn[0], LV_SYMBOL_MINUS);
        set_btn_text(s_fe_btn[1], LV_SYMBOL_PLUS);
    }
    for (int k = 0; k < 2; k++) {
        lv_obj_set_style_text_font(lv_obj_get_child(s_fe_btn[k], 0), big, 0);
    }

    char range[64], def[24], defp[32], info[128];
    cfgtext_range(f, range, sizeof(range));
    cfgall_get_default(idx, def, sizeof(def));
    pretty(f, def, defp, sizeof(defp));
    if (range[0]) {
        snprintf(info, sizeof(info), "Range %s\nFactory setting: %s", range, defp);
    } else if (f->type == CT_TIME_OPT) {
        snprintf(info, sizeof(info), "Steps of 15 min, below 00:00 = not used\nFactory setting: %s", defp);
    } else if (f->type == CT_TIME) {
        snprintf(info, sizeof(info), "Steps of 15 min\nFactory setting: %s", defp);
    } else {
        snprintf(info, sizeof(info), "Factory setting: %s", defp);
    }
    lv_label_set_text(s_fe_info, info);
    fe_show();
    lv_screen_load(s_fe_scr);
}

/* -------------------------------------------------------- boiler card */

static lv_obj_t *s_bl_icon, *s_bl_icon_sym, *s_bl_state, *s_bl_sub, *s_bl_flow, *s_bl_mod_bar, *s_bl_gas, *s_bl_alerts;
static lv_obj_t *s_bl_val[6], *s_bl_note[6];

static lv_obj_t *card(lv_obj_t *p, int x, int y, int w, int h)
{
    lv_obj_t *c = lv_obj_create(p);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, ui_c(UI_SURFACE), 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return c;
}

void ui_pages_boiler(lv_obj_t *p)
{
    label(p, "Boiler", 20, 24, 0, UI_TEXT, &lv_font_montserrat_28);
    // State card: icon, what the boiler does, water temperature
    lv_obj_t *c = card(p, 20, 84, 440, 112);
    s_bl_icon = lv_obj_create(c);
    lv_obj_remove_style_all(s_bl_icon);
    lv_obj_set_size(s_bl_icon, 72, 72);
    lv_obj_set_pos(s_bl_icon, 16, 20);
    lv_obj_set_style_radius(s_bl_icon, 36, 0);
    lv_obj_set_style_bg_opa(s_bl_icon, LV_OPA_COVER, 0);
    s_bl_icon_sym = lv_label_create(s_bl_icon);
    lv_obj_set_style_text_font(s_bl_icon_sym, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_bl_icon_sym, lv_color_white(), 0);
    lv_label_set_text(s_bl_icon_sym, LV_SYMBOL_CHARGE);
    lv_obj_center(s_bl_icon_sym);
    s_bl_state = label(c, "", 104, 18, 230, UI_TEXT, &lv_font_montserrat_28);
    s_bl_sub = label(c, "", 104, 70, 330, UI_MUTED, &lv_font_montserrat_14);
    s_bl_flow = label(c, "", 300, 18, 124, UI_VALUE, &lv_font_montserrat_40);
    lv_obj_set_style_text_align(s_bl_flow, LV_TEXT_ALIGN_RIGHT, 0);

    // Tiles
    static const char *titles[6] = {"Water out / back", "Burner power", "Water pressure",
                                    "Hot water tank",   "Burner starts", "Efficiency (24 h)"};
    for (int i = 0; i < 6; i++) {
        lv_obj_t *t = card(p, 20 + (i % 2) * 228, 208 + (i / 2) * 118, 212, 108);
        label(t, titles[i], 14, 10, 190, UI_MUTED, &lv_font_montserrat_14);
        s_bl_val[i] = label(t, "-", 14, 32, 190, UI_TEXT, &lv_font_montserrat_28);
        s_bl_note[i] = label(t, "", 14, 76, 190, UI_MUTED, &lv_font_montserrat_14);
        if (i == 1) {
            s_bl_mod_bar = lv_bar_create(t);
            lv_obj_set_size(s_bl_mod_bar, 184, 10);
            lv_obj_set_pos(s_bl_mod_bar, 14, 70);
            lv_bar_set_range(s_bl_mod_bar, 0, 100);
            lv_obj_set_style_bg_color(s_bl_mod_bar, ui_c(UI_SURFACE_ALT), LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_bl_mod_bar, ui_c(UI_HEAT), LV_PART_INDICATOR);
            lv_obj_set_y(s_bl_note[i], 84);
        }
    }
    s_bl_gas = label(p, "", 20, 562, 440, UI_MUTED, &lv_font_montserrat_14);
    s_bl_alerts = label(p, "", 20, 606, 440, UI_OK, NULL);
}

// Plain words for the controller state and the hot water reason (the internal names go to MQTT and logs).
static const char *ctrl_words(ctrl_state_t s)
{
    static const char *const n[] = {"idle",          "on/off heating",     "modulating",      "keeping small flame",
                                    "pulsing: burn", "pulsing: pause", "sensor lost: gentle heat", "frost protection"};
    return s <= CTRL_ST_FROST ? n[s] : "?";
}

static const char *dhw_words(sched_dhw_reason_t r)
{
    static const char *const n[] = {"allowed",      "allowed (window)",   "on (tank too cold)", "on (after heating)",
                                    "on (boost)",   "on (hot cycle)",     "allowed (no clock)", "off (outside windows)",
                                    "off",          "off (away)"};
    return r <= DHW_R_AWAY ? n[r] : "?";
}

void ui_pages_boiler_update(void)
{
    thermo_state_t st;
    thermo_get(&st);
    dl_kpi_t k1, k24;
    thermo_get_kpi(&k1, &k24);
    alert_config_t ac;
    thermo_get_alert_config(&ac);

    // State card
    uint32_t col = UI_FAINT;
    const char *state = "Idle";
    if (!st.boiler_online) {
        col = UI_ERR, state = "Not answering";
    } else if (st.flame && st.dhw_active) {
        col = 0x00838F, state = "Hot water";
    } else if (st.flame) {
        col = UI_HEAT, state = "Heating";
    } else if (st.demand) {
        col = UI_WARN, state = "Waiting";
    }
    lv_obj_set_style_bg_color(s_bl_icon, ui_c(col), 0);
    lv_label_set_text(s_bl_state, state);
    char b[128];
    if (st.demand) {
        snprintf(b, sizeof(b), "wants %.0f °C, %s\n%s boiler, %s", st.flow_setpoint, ctrl_words(st.ctrl_state),
                 st.boiler_sim ? "simulated" : "real", st.boiler_online ? "answering" : "no answer");
    } else {
        snprintf(b, sizeof(b), "no heat needed now\n%s boiler, %s", st.boiler_sim ? "simulated" : "real",
                 st.boiler_online ? "answering" : "no answer");
    }
    lv_label_set_text(s_bl_sub, b);
    if (st.boiler_online) {
        lv_label_set_text_fmt(s_bl_flow, "%.0f°", st.flow_c);
    } else {
        lv_label_set_text(s_bl_flow, "");
    }

    // Tiles
    lv_label_set_text_fmt(s_bl_val[0], "%.0f / %.0f °C", st.flow_c, st.return_c);
    lv_label_set_text_fmt(s_bl_note[0], "difference %.0f °C", st.flow_c - st.return_c);
    float mod = st.flame ? (st.modulation > 100 ? 100 : st.modulation) : 0;
    lv_label_set_text_fmt(s_bl_val[1], st.flame ? "%.0f %%" : "off", mod);
    lv_bar_set_value(s_bl_mod_bar, (int32_t)mod, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_bl_note[1], "24 h average %.0f %%", k24.mean_mod);
    if (st.press_valid) {
        bool low = st.pressure_bar < ac.pressure_min_bar;
        lv_label_set_text_fmt(s_bl_val[2], "%.2f bar", st.pressure_bar);
        lv_obj_set_style_text_color(s_bl_val[2], ui_c(low ? UI_ERR : UI_TEXT), 0);
        lv_label_set_text(s_bl_note[2], low ? "low: top up the water" : "normal (1-2 bar)");
    } else {
        lv_label_set_text(s_bl_val[2], "-");
        lv_label_set_text(s_bl_note[2], "not reported");
    }
    if (st.tank_valid) {
        lv_label_set_text_fmt(s_bl_val[3], "%.0f °C", st.tank_c);
    } else {
        lv_label_set_text(s_bl_val[3], "-");
    }
    lv_label_set_text(s_bl_note[3], dhw_words(st.dhw_reason));
    lv_label_set_text_fmt(s_bl_val[4], "%u last hour", k1.ch_starts);
    if (k24.ch_burns) {
        lv_label_set_text_fmt(s_bl_note[4], "24 h: %u, typical burn %.0f min", k24.ch_starts, k24.median_burn_min);
    } else {
        lv_label_set_text_fmt(s_bl_note[4], "24 h: %u starts", k24.ch_starts);
    }
    if (k24.burner_h > 0) {
        lv_label_set_text_fmt(s_bl_val[5], "%.0f %%", k24.cond_pct);
        lv_label_set_text(s_bl_note[5], "of the time condensing");
    } else {
        lv_label_set_text(s_bl_val[5], "-");
        lv_label_set_text(s_bl_note[5], "no burns yet");
    }

    lv_label_set_text_fmt(s_bl_gas, "Gas used (estimate): %.1f kWh in 24 h, %.1f kWh last hour", k24.gas_kwh,
                          k1.gas_kwh);
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

// Colour of a level: Comfort orange, Eco blue, fixed temperatures violet (lighter = cooler, deeper = warmer).
static uint32_t level_color(uint8_t level)
{
    if (!sched_level_is_fixed(level)) {
        return level == SCHED_COMFORT ? C_COMFORT : C_ECO;
    }
    float f = (level / 4.0f - 16) / 7;
    f = f < 0 ? 0 : (f > 1 ? 1 : f);
    uint32_t a = C_FIXED_LO, b = C_FIXED_HI, out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
        out |= (uint32_t)(ca + (cb - ca) * f) << sh;
    }
    return out;
}

// "Comfort", "Eco" or "21.5°"
static void level_label(uint8_t level, char *out, size_t size)
{
    if (sched_level_is_fixed(level)) {
        float c = level / 4.0f;
        snprintf(out, size, c == (int)c ? "%.0f °C" : "%.1f °C", c);
    } else {
        snprintf(out, size, "%s", level == SCHED_COMFORT ? "Comfort" : "Eco");
    }
}

static void draw_day(int d, const sched_config_t *c)
{
    lv_obj_t *bar = s_week[d];
    lv_obj_clean(bar);
    lv_obj_set_style_bg_color(bar, ui_c(UI_SURFACE_ALT), 0);
    int w = lv_obj_get_width(bar);
    // Walk the day in 15-minute steps and draw one segment per run of the same level
    int start = 0;
    uint8_t cur = (uint8_t)sched_level_at(c, d, 0);
    for (int m = 15; m <= 1440; m += 15) {
        uint8_t l = m < 1440 ? (uint8_t)sched_level_at(c, d, m) : 0xFF;
        if (l == cur) {
            continue;
        }
        lv_obj_t *r = lv_obj_create(bar);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_set_style_radius(r, 4, 0);
        lv_obj_set_style_pad_all(r, 0, 0);
        lv_obj_set_style_bg_color(r, c->prog_on ? lv_color_hex(level_color(cur)) : ui_c(UI_FAINT), 0);
        lv_obj_set_style_bg_opa(r, c->prog_on ? LV_OPA_COVER : (cur == SCHED_ECO ? LV_OPA_20 : LV_OPA_60), 0);
        int x0 = start * w / 1440, x1 = m * w / 1440;
        lv_obj_set_pos(r, x0, 0);
        lv_obj_set_size(r, x1 - x0, LV_PCT(100));
        if (sched_level_is_fixed(cur) && x1 - x0 >= 44) {
            char t[12];
            level_label(cur, t, sizeof(t));
            lv_obj_t *lt = lv_label_create(r);
            lv_obj_set_style_text_font(lt, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(lt, lv_color_white(), 0);
            lv_label_set_text(lt, t);
            lv_obj_center(lt);
        }
        start = m;
        cur = l;
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
    label(p, "Schedule", 20, 24, 0, UI_TEXT, &lv_font_montserrat_28);
    static const char *hours[5] = {"0", "6", "12", "18", "24"};
    for (int h = 0; h < 5; h++) {
        lv_obj_t *l = label(p, hours[h], 0, 78, 0, UI_FAINT, NULL);
        lv_obj_update_layout(l);
        lv_obj_set_x(l, 76 + h * 96 - lv_obj_get_width(l) / 2);
    }
    for (int d = 0; d < 7; d++) {
        label(p, DAYS[d], 20, 106 + d * 44, 0, UI_MUTED, NULL);
        lv_obj_t *bar = lv_obj_create(p);
        lv_obj_set_pos(bar, 76, 100 + d * 44);
        lv_obj_set_size(bar, 384, 34);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 6, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(bar, day_cb, LV_EVENT_CLICKED, (void *)(intptr_t)d);
        s_week[d] = bar;
    }
    s_prog_btn = button(p, "", 0x37474F, prog_cb, NULL, 20, 414, 440, 52);
    s_boost_btn = button(p, "", 0xD84315, boost_cb, NULL, 20, 476, 212, 64);
    s_away_btn = button(p, "", 0x37474F, away_cb, NULL, 248, 476, 212, 64);
    label(p, "Hot water", 20, 548, 0, UI_MUTED, NULL);
    s_dhw_btn = button(p, "", 0x37474F, dhw_mode_cb, NULL, 20, 574, 212, 60);
    s_dhwb_btn = button(p, "", 0x00838F, dhw_boost_cb, NULL, 248, 574, 212, 60);
    s_sched_info = label(p, "", 20, 642, 440, UI_MUTED, &lv_font_montserrat_14);
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
    static const char *const pres[] = {"not run yet", "finished",        "timed out", "boiler ignored the temperature",
                                       "no tank reading", "no tank temperature", "cancelled"};
    const char *pr = st.pasteur_result <= PASTEUR_CANCELLED ? pres[st.pasteur_result] : "?";
    lv_label_set_text_fmt(s_sched_info, "Hot water %s, %s.\nWeekly hot cycle: %s%s.", dhw_words(st.dhw_reason), tank,
                          c.pasteur_on ? "on, last " : "off", c.pasteur_on ? pr : "");
}

/* --------------------------------------------------------- day editor */

static lv_obj_t *s_de_scr, *s_de_title, *s_de_row[SCHED_POINTS], *s_de_time[SCHED_POINTS], *s_de_lvl[SCHED_POINTS];
static lv_obj_t *s_de_copy[7], *s_de_msg;
static lv_obj_t *s_lp, *s_lp_title, *s_lp_btn[3], *s_lp_temp;
static int s_lp_row;
static uint8_t s_lp_fixed;  // the fixed temperature shown in the picker (level units)
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
        char t[16];
        level_label(s_de_pts[i].level, t, sizeof(t));
        set_btn_text(s_de_lvl[i], t);
        lv_obj_set_style_bg_color(s_de_lvl[i], lv_color_hex(level_color(s_de_pts[i].level)), 0);
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

// Level picker for one switch time: Comfort, Eco, or a fixed temperature.
static void lp_show(void)
{
    thermo_state_t st;
    thermo_get(&st);
    sched_config_t c;
    thermo_get_sched_config(&c);
    uint8_t cur = s_de_pts[s_lp_row].level;
    lv_label_set_text_fmt(s_lp_title, "From %02d:%02d the room should be:", s_de_pts[s_lp_row].min / 60,
                          s_de_pts[s_lp_row].min % 60);
    char b[64];
    snprintf(b, sizeof(b), "Comfort\n#B0BEC5 the dial: now %.1f °C#", st.setpoint);
    ui_label_text(lv_obj_get_child(s_lp_btn[0], 0), b);
    snprintf(b, sizeof(b), "Eco\n#B0BEC5 dial - %.1f: now %.1f °C#", c.eco_setback_k, st.setpoint - c.eco_setback_k);
    ui_label_text(lv_obj_get_child(s_lp_btn[1], 0), b);
    snprintf(b, sizeof(b), "Fixed temperature");
    set_btn_text(s_lp_btn[2], b);
    float f = s_lp_fixed / 4.0f;
    lv_label_set_text_fmt(s_lp_temp, f == (int)f ? "%.0f °C" : "%.1f °C", f);
    for (int k = 0; k < 3; k++) {
        bool on = (k == 0 && cur == SCHED_COMFORT) || (k == 1 && cur == SCHED_ECO) || (k == 2 && sched_level_is_fixed(cur));
        lv_obj_set_style_border_width(s_lp_btn[k], on ? 4 : 0, 0);
    }
}

static void lp_pick_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    s_de_pts[s_lp_row].level = k == 0 ? SCHED_COMFORT : (k == 1 ? SCHED_ECO : s_lp_fixed);
    lp_show();
}

static void lp_temp_cb(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    int v = s_lp_fixed + dir * 2;  // 0.5 °C steps
    s_lp_fixed = (uint8_t)(v < SCHED_FIXED_MIN ? SCHED_FIXED_MIN : (v > SCHED_FIXED_MAX ? SCHED_FIXED_MAX : v));
    s_de_pts[s_lp_row].level = s_lp_fixed;  // changing the temperature picks "fixed"
    lp_show();
}

static void lp_done_cb(lv_event_t *e)
{
    lv_obj_add_flag(s_lp, LV_OBJ_FLAG_HIDDEN);
    de_show();
}

static void de_lvl_cb(lv_event_t *e)
{
    s_lp_row = (int)(intptr_t)lv_event_get_user_data(e);
    uint8_t cur = s_de_pts[s_lp_row].level;
    if (sched_level_is_fixed(cur)) {
        s_lp_fixed = cur;
    } else {
        thermo_state_t st;
        thermo_get(&st);
        s_lp_fixed = sched_level_from_c(roundf(st.setpoint * 2) / 2);  // start from the dial
    }
    lp_show();
    lv_obj_remove_flag(s_lp, LV_OBJ_FLAG_HIDDEN);
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
                                  n && s_de_pts[n - 1].level != SCHED_ECO ? SCHED_ECO : SCHED_COMFORT};
    de_show();
}

static void de_save_cb(lv_event_t *e)
{
    sched_config_t c;
    thermo_get_sched_config(&c);
    memcpy(c.prog[s_de_day], s_de_pts, sizeof(s_de_pts));
    char text[120];
    sched_prog_format(&c, s_de_day, text, sizeof(text));  // through the parser: sorted and checked
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
            lv_obj_set_style_text_color(s_de_lvl[i], lv_color_white(), 0);
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
        // Level picker (over the editor)
        s_lp = ui_overlay(c);
        s_lp_title = label(s_lp, "", 20, 16, 440, UI_TEXT, &lv_font_montserrat_28);
        for (int k = 0; k < 3; k++) {
            uint32_t col = k == 0 ? C_COMFORT : (k == 1 ? C_ECO : C_FIXED);
            s_lp_btn[k] = button(s_lp, "", col, lp_pick_cb, (void *)(intptr_t)k, 20, 100 + k * 104, 440, 92);
            lv_obj_set_style_border_color(s_lp_btn[k], ui_c(UI_TEXT), 0);
            lv_label_set_recolor(lv_obj_get_child(s_lp_btn[k], 0), true);
            lv_obj_set_style_text_align(lv_obj_get_child(s_lp_btn[k], 0), LV_TEXT_ALIGN_CENTER, 0);
        }
        lv_obj_t *mb = button(s_lp, LV_SYMBOL_MINUS, 0x1565C0, lp_temp_cb, (void *)(intptr_t)-1, 20, 420, 120, 84);
        lv_obj_add_event_cb(mb, lp_temp_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)-1);
        lv_obj_t *pb = button(s_lp, LV_SYMBOL_PLUS, 0xD84315, lp_temp_cb, (void *)(intptr_t)1, 340, 420, 120, 84);
        lv_obj_add_event_cb(pb, lp_temp_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)1);
        s_lp_temp = label(s_lp, "", 140, 440, 200, UI_VALUE, &lv_font_montserrat_40);
        lv_obj_set_style_text_align(s_lp_temp, LV_TEXT_ALIGN_CENTER, 0);
        label(s_lp, "Changing the temperature selects \"Fixed temperature\".", 20, 516, 440, UI_MUTED,
              &lv_font_montserrat_14);
        button(s_lp, LV_SYMBOL_OK " Done", 0x2E7D32, lp_done_cb, NULL, 20, 600, 440, 72);
    }
    lv_obj_add_flag(s_lp, LV_OBJ_FLAG_HIDDEN);
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

/* ------------------------------------------------- all settings by topic */

#define GRP_MAX 24
static lv_obj_t *s_adv_scr, *s_grp_scr, *s_grp_title, *s_grp_list, *s_grp_val[GRP_MAX];
static int s_grp_idx[GRP_MAX], s_grp_n;

static void grp_refresh(void)
{
    for (int i = 0; i < s_grp_n; i++) {
        row_refresh(s_grp_val[i], s_grp_idx[i]);
    }
}

static void grp_row_cb(lv_event_t *e)
{
    open_field(s_grp_idx[(int)(intptr_t)lv_event_get_user_data(e)], s_grp_scr, grp_refresh);
}

static void grp_back_cb(lv_event_t *e)
{
    lv_screen_load(s_adv_scr);
}

static void group_open(cfg_group_t g)
{
    if (!s_grp_scr) {
        lv_obj_t *c;
        s_grp_scr = screen("Settings", &c, grp_back_cb);
        s_grp_list = lv_obj_create(c);
        lv_obj_set_size(s_grp_list, 480, UI_CONTENT_H);
        lv_obj_set_style_bg_opa(s_grp_list, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_grp_list, 0, 0);
        lv_obj_set_style_pad_hor(s_grp_list, 20, 0);
        lv_obj_set_style_pad_ver(s_grp_list, 12, 0);
        lv_obj_set_flex_flow(s_grp_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(s_grp_list, 8, 0);
    }
    lv_obj_clean(s_grp_list);
    s_grp_title = lv_label_create(s_grp_list);
    lv_obj_set_style_text_color(s_grp_title, ui_c(UI_TEXT), 0);
    lv_obj_set_style_text_font(s_grp_title, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_grp_title, cfgtext_group_title(g));
    lv_obj_t *hint = lv_label_create(s_grp_list);
    lv_obj_set_width(hint, 440);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(hint, ui_c(UI_MUTED), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(hint, "Tap a setting to see what it does and change it. Indented settings belong to the one "
                            "above them; dimmed ones are not in use now.");
    s_grp_n = 0;
    for (int i = 0; i < cfgtext_count() && s_grp_n < GRP_MAX; i++) {
        const cfgtext_t *t = cfgtext_at(i);
        int k = t->group == g ? cfgall_find(t->name) : -1;
        if (k < 0) {
            continue;
        }
        int indent = t->parent ? 24 : 0;
        s_grp_idx[s_grp_n] = k;
        s_grp_val[s_grp_n] = setting_row(s_grp_list, k, indent, -1, 440 - indent, grp_row_cb, (void *)(intptr_t)s_grp_n);
        s_grp_n++;
    }
    grp_refresh();
    lv_obj_scroll_to_y(s_grp_list, 0, LV_ANIM_OFF);
    lv_screen_load(s_grp_scr);
}

static void adv_group_cb(lv_event_t *e)
{
    group_open((cfg_group_t)(intptr_t)lv_event_get_user_data(e));
}

static void advanced_open(void)
{
    if (!s_adv_scr) {
        lv_obj_t *c;
        s_adv_scr = screen("All settings", &c, NULL);
        for (int g = 0; g < CG_COUNT; g++) {
            lv_obj_t *b = lv_button_create(c);
            lv_obj_set_pos(b, 20, 12 + g * 74);
            lv_obj_set_size(b, 440, 66);
            lv_obj_set_style_bg_color(b, ui_c(UI_SURFACE), 0);
            lv_obj_set_style_radius(b, 8, 0);
            lv_obj_set_style_pad_all(b, 0, 0);
            lv_obj_add_event_cb(b, adv_group_cb, LV_EVENT_CLICKED, (void *)(intptr_t)g);
            lv_obj_t *n = lv_label_create(b);
            lv_obj_set_style_text_color(n, ui_c(UI_TEXT), 0);
            lv_label_set_text(n, cfgtext_group_title(g));
            lv_obj_set_pos(n, 12, 9);
            lv_obj_t *d = lv_label_create(b);
            lv_obj_set_style_text_font(d, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(d, ui_c(UI_MUTED), 0);
            lv_label_set_text(d, cfgtext_group_brief(g));
            lv_obj_set_pos(d, 12, 39);
            lv_obj_t *a = lv_label_create(b);
            lv_obj_set_style_text_color(a, ui_c(UI_MUTED), 0);
            lv_label_set_text(a, LV_SYMBOL_RIGHT);
            lv_obj_align(a, LV_ALIGN_RIGHT_MID, -12, 0);
        }
    }
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
    char dots[sizeof(s_pin) * 2 + 1] = "";  // two characters per digit
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

// Header "Back": from a routine's detail to the routine list, from the list to the Settings page.
static void sv_back_cb(lv_event_t *e)
{
    if (s_sv_detail && !lv_obj_has_flag(s_sv_detail, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_sv_detail, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_screen_load(s_thermo_scr);
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
        lv_obj_t *box = lv_msgbox_create(s_sv_scr);  // on the service screen: gone when the screen changes
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
        s_sv_scr = screen("Service", &c, sv_back_cb);
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

static const char *EVERYDAY[] = {"prog_on", "eco_setback_k", "dhw_mode", "summer_on", "frost_on", "frost_room_c",
                                 "loss_mode", "ufh_on"};
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
    label(p, "Settings", 20, 14, 0, UI_TEXT, &lv_font_montserrat_28);
    label(p, "Tap a row to see what it does and change it.", 20, 52, 310, UI_MUTED, &lv_font_montserrat_14);
    for (int i = 0; i < N_EVERYDAY; i++) {
        int k = cfgall_find(EVERYDAY[i]);
        s_ev_val[i] = setting_row(p, k, 20, 76 + i * 70, 440, ev_cb, (void *)(intptr_t)i);
    }
    button(p, LV_SYMBOL_LIST " All settings", 0x37474F, adv_cb, NULL, 20, 642, 212, 60);
    button(p, LV_SYMBOL_SETTINGS " Service", 0x6A1B9A, svc_cb, NULL, 248, 642, 212, 60);
}

void ui_pages_settings_update(void)
{
    for (int i = 0; i < N_EVERYDAY; i++) {
        int k = cfgall_find(EVERYDAY[i]);
        if (k >= 0) {
            row_refresh(s_ev_val[i], k);
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
    } else if (strncmp(what, "group ", 6) == 0) {
        group_open((cfg_group_t)(atoi(what + 6) % CG_COUNT));
    } else if (strcmp(what, "service") == 0) {
        service_open();
    } else if (strncmp(what, "day ", 4) == 0) {
        day_editor_open(atoi(what + 4) % 7);
    }
}
