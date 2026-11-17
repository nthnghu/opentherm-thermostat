// UI shell: status bar on the top layer, app launcher, inactivity return to the thermostat, dimming.
#include "ui.h"
#include "ui_pages.h"
#include "ui_theme.h"
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include "board.h"
#include "esp_timer.h"
#include "settings.h"
#include "thermostat.h"
#include "timekeep.h"
#include "selftest.h"
#include "ha.h"
#include "quickpanel.h"

enum { APP_THERMO, APP_HA, APP_CALC, APP_BLOCKS, APP_SETUP, APP_COUNT };

static lv_obj_t *s_apps[APP_COUNT];
static lv_obj_t *s_launcher;
static lv_obj_t *s_clock, *s_heat, *s_warn, *s_ha, *s_wifi, *s_bars[4];
static lv_obj_t *s_wake;  // full-screen catcher while dimmed: the first touch only wakes the screen
static bool s_busy;

static void load(lv_obj_t *scr, lv_screen_load_anim_t anim);
static void back_cb(lv_event_t *e);
static int s_bl = -1;
static bool s_forced_off;     // "Screen off now" from the quick panel
static uint32_t s_forced_at;  // lv_tick at that moment

/* ---------------------------------------------------------------- widgets */

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_color_t color, lv_event_cb_t cb, void *user)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    }
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return btn;
}

lv_obj_t *ui_overlay(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_bg_color(o, ui_c(0x121212), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static lv_obj_t *plain_container(lv_obj_t *scr, int y, int h)
{
    lv_obj_t *c = lv_obj_create(scr);
    lv_obj_set_size(c, BOARD_LCD_H_RES, h);
    lv_obj_set_pos(c, 0, y);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

lv_obj_t *ui_plain_screen(lv_obj_t **content)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, ui_c(UI_BG), 0);
    *content = plain_container(scr, UI_STATUS_H, BOARD_LCD_V_RES - UI_STATUS_H);
    return scr;
}

static void back_cb(lv_event_t *e)
{
    ui_go_launcher();
}

static void back_to_cb(lv_event_t *e)
{
    load(lv_event_get_user_data(e), LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

lv_obj_t *ui_sub_screen(const char *title, lv_obj_t **content, lv_obj_t *parent_screen)
{
    lv_obj_t *scr = ui_app_screen(title, content);
    // Re-point the header's back button at the parent screen.
    lv_obj_t *header = lv_obj_get_child(scr, 0);
    lv_obj_t *back = lv_obj_get_child(header, 0);
    lv_obj_remove_event_cb(back, back_cb);
    lv_obj_add_event_cb(back, back_to_cb, LV_EVENT_CLICKED, parent_screen);
    lv_label_set_text(lv_obj_get_child(back, 0), LV_SYMBOL_LEFT " Back");
    return scr;
}

lv_obj_t *ui_app_screen(const char *title, lv_obj_t **content)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, ui_c(UI_BG), 0);

    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, LV_PCT(100), UI_HEADER_H);
    lv_obj_set_pos(header, 0, UI_STATUS_H);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_bg_color(header, ui_c(0x1E2A38), 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = ui_button(header, LV_SYMBOL_LEFT " Apps", ui_c(0x37474F), back_cb, NULL);
    lv_obj_set_size(back, 120, 48);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, -6, 0);

    lv_obj_t *t = lv_label_create(header);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_CENTER, 50, 0);

    *content = plain_container(scr, UI_STATUS_H + UI_HEADER_H, UI_CONTENT_H);
    return scr;
}

/* ------------------------------------------------------------ navigation */

static void load(lv_obj_t *scr, lv_screen_load_anim_t anim)
{
    if (lv_screen_active() != scr) {
        lv_screen_load_anim(scr, anim, 180, 0, false);
    }
}

void ui_go_launcher(void)
{
    load(s_launcher, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

void ui_go_thermostat(void)
{
    load(s_apps[APP_THERMO], LV_SCR_LOAD_ANIM_FADE_IN);
}

void ui_open_setup(void)
{
    load(s_apps[APP_SETUP], LV_SCR_LOAD_ANIM_NONE);
}

void ui_set_busy(bool busy)
{
    s_busy = busy;
}

static void open_app_cb(lv_event_t *e)
{
    load(lv_event_get_user_data(e), LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

/* ------------------------------------------------------------ status bar */

static void status_cb(lv_timer_t *t)
{
    // Clock. The thermostat's main page shows its own large clock, so the status bar shows only a hint there.
    unsigned long muted = (unsigned long)ui_hex(UI_MUTED);
    bool thermo = lv_screen_active() == s_apps[APP_THERMO] && app_thermo_page() == 0;
    if (timekeep_valid()) {
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        bool manual = timekeep_source() == TIME_SRC_MANUAL;
        if (thermo) {
            lv_label_set_text_fmt(s_clock, manual ? "#%06lX manual clock#" : "", muted);
        } else {
            lv_label_set_text_fmt(s_clock, manual ? "%02d:%02d  #%06lX manual#" : "%02d:%02d", lt.tm_hour, lt.tm_min, muted);
        }
    } else {
        lv_label_set_text_fmt(s_clock, thermo ? "#%06lX clock not set#" : "--:--  #%06lX clock not set#", muted);
    }

    thermo_state_t st;
    thermo_get(&st);
    if (st.demand) {
        lv_obj_remove_flag(s_heat, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_heat, LV_OBJ_FLAG_HIDDEN);
    }
    if (!st.sensor_ok || st.sensor_age_s >= 6 || !selftest_required_ok()) {
        lv_obj_remove_flag(s_warn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_warn, LV_OBJ_FLAG_HIDDEN);
    }

    ha_state_t hs = ha_state();
    lv_obj_set_style_text_color(s_ha, ui_c(hs == HA_STATE_OK ? UI_OK : hs == HA_STATE_ERROR ? UI_ERR : UI_FAINT), 0);

    // Wi-Fi: four signal bars (the dBm figure is in the quick panel); "off" when not connected
    int rssi = board_wifi_rssi();
    bool up = board_wifi_connected();
    int n = !up ? 0 : rssi > -60 ? 4 : rssi > -68 ? 3 : rssi > -78 ? 2 : 1;
    uint32_t col = n >= 3 ? UI_OK : n == 2 ? UI_WARN : UI_ERR;
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(s_bars[i], ui_c(i < n ? col : UI_DOT_OFF), 0);
    }
    lv_obj_set_hidden(s_wifi, up);
}

static void bar_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_GESTURE && lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_BOTTOM) {
        return;
    }
    quickpanel_open();
}

static void create_status_bar(void)
{
    lv_obj_t *bar = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bar, BOARD_LCD_H_RES, UI_STATUS_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, ui_c(UI_STATUSBAR), 0);
    lv_obj_set_style_pad_hor(bar, 12, 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);  // tap or swipe down: quick panel
    lv_obj_add_event_cb(bar, bar_cb, LV_EVENT_CLICKED, NULL);
    // Gestures stop here (LVGL passes them up to the top layer otherwise); a swipe is no click (swipe guard).
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(bar, bar_cb, LV_EVENT_GESTURE, NULL);

    s_clock = lv_label_create(bar);
    lv_label_set_recolor(s_clock, true);
    lv_obj_set_style_text_color(s_clock, ui_c(UI_TEXT), 0);  // the top layer doesn't inherit the theme's text colour
    lv_obj_align(s_clock, LV_ALIGN_LEFT_MID, 0, 0);

    s_heat = lv_label_create(bar);
    lv_obj_set_style_text_color(s_heat, ui_c(UI_HEAT), 0);
    lv_label_set_text(s_heat, LV_SYMBOL_CHARGE " HEAT");
    lv_obj_align(s_heat, LV_ALIGN_CENTER, 10, 0);

    for (int i = 0; i < 4; i++) {
        s_bars[i] = lv_obj_create(bar);
        lv_obj_remove_style_all(s_bars[i]);
        lv_obj_set_size(s_bars[i], 5, 6 + i * 4);
        lv_obj_set_style_radius(s_bars[i], 1, 0);
        lv_obj_set_style_bg_opa(s_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_bars[i], ui_c(UI_DOT_OFF), 0);
        lv_obj_align(s_bars[i], LV_ALIGN_BOTTOM_RIGHT, -(3 - i) * 8, -9);
    }
    s_wifi = lv_label_create(bar);  // "off", left of the empty bars
    lv_obj_set_style_text_color(s_wifi, ui_c(UI_FAINT), 0);
    lv_label_set_text(s_wifi, "off");
    lv_obj_align(s_wifi, LV_ALIGN_RIGHT_MID, -38, 0);
    s_ha = lv_label_create(bar);
    lv_label_set_text(s_ha, LV_SYMBOL_HOME);
    lv_obj_align(s_ha, LV_ALIGN_RIGHT_MID, -78, 0);
    s_warn = lv_label_create(bar);
    lv_obj_set_style_text_color(s_warn, ui_c(UI_ERR), 0);
    lv_label_set_text(s_warn, LV_SYMBOL_WARNING);
    lv_obj_align(s_warn, LV_ALIGN_RIGHT_MID, -116, 0);

    status_cb(NULL);
    lv_timer_create(status_cb, 1000, NULL);
}

/* -------------------------------------------------- inactivity + backlight */

// True when the current local time is inside [start, end) (minutes after midnight; may wrap midnight).
static bool in_window(bool on, uint16_t start, uint16_t end)
{
    if (!on || !timekeep_valid()) {
        return false;
    }
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    int m = lt.tm_hour * 60 + lt.tm_min;
    return start <= end ? (m >= start && m < end) : (m >= start || m < end);
}

static void wake_cb(lv_event_t *e)
{
    // Swallowed press; the next idle tick sees fresh activity and restores the brightness.
}

// Screen states from inactivity (touch, or camera motion when enabled):
//   on  -> dimmed after dim_after_s -> off after off_after_s, unless inside the "keep on" window.
// The night window lowers the normal brightness. Dimmed or off, the first touch only wakes the screen.
static void idle_cb(lv_timer_t *t)
{
    const settings_t *c = settings_get();
    uint32_t idle_s = lv_display_get_inactive_time(NULL) / 1000;

    // Back to the thermostat's main page: from another app, from an editor screen, or from pages 2-5
    if (c->home_after_s && idle_s >= c->home_after_s && !s_busy) {
        if (lv_screen_active() != s_apps[APP_THERMO]) {
            ui_go_thermostat();
        }
        if (app_thermo_page() != 0) {
            app_thermo_show_page(0);
        }
    }

    ui_theme_update();

    // "Screen off now" lasts until there is activity after it (touch or camera motion).
    if (s_forced_off) {
        uint32_t since = lv_tick_elaps(s_forced_at);
        if (since > 1000 && lv_display_get_inactive_time(NULL) + 500 < since) {
            s_forced_off = false;
        }
    }

    int level = in_window(c->night_on, c->night_start, c->night_end) ? c->night_level : c->bright;
    bool off = s_forced_off || (c->off_after_s && idle_s >= c->off_after_s && !s_busy &&
                                !in_window(c->keep_on, c->keep_on_start, c->keep_on_end));
    bool dim = c->dim_after_s && idle_s >= c->dim_after_s && !s_busy;
    if (off) {
        level = 0;
    } else if (dim && c->dim_level < level) {
        level = c->dim_level;
    }
    if (level != s_bl) {
        s_bl = level;
        board_backlight(level);
    }
    bool asleep = off || dim;
    if (asleep && !s_wake) {
        s_wake = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_wake);
        lv_obj_set_size(s_wake, LV_PCT(100), LV_PCT(100));
        lv_obj_add_flag(s_wake, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_wake, wake_cb, LV_EVENT_PRESSED, NULL);
    } else if (!asleep && s_wake) {
        lv_obj_delete(s_wake);
        s_wake = NULL;
    }
}

void ui_screen_off_now(void)
{
    s_forced_off = true;
    s_forced_at = lv_tick_get();
    idle_cb(NULL);
}

// Auto: HA's sun.sun when known, else the day window (needs a valid clock), else keep the current theme.
void ui_theme_update(void)
{
    const settings_t *c = settings_get();
    bool dark;
    if (c->theme_mode == 1) {
        dark = false;
    } else if (c->theme_mode == 2) {
        dark = true;
    } else if (c->theme_mode == 3) {  // by the hours only
        dark = timekeep_valid() ? !in_window(true, c->day_start, c->day_end) : ui_theme_is_dark();
    } else {
        bool up;
        // HA set up but its sun state not in yet (first ~10 s after a start): keep the theme we had, for up to
        // 2 min, instead of guessing from the hours first and flipping again when HA answers.
        bool ha_set = c->ha_url[0] && c->ha_token[0];
        if (ha_sun_is_up(&up)) {
            dark = !up;
        } else if (ha_set && esp_timer_get_time() < 120LL * 1000000) {
            dark = ui_theme_is_dark();
        } else if (timekeep_valid()) {
            dark = !in_window(true, c->day_start, c->day_end);
        } else {
            dark = ui_theme_is_dark();
        }
    }
    ui_theme_apply(dark);
}

void ui_open_setup_section(int section)
{
    load(s_apps[APP_SETUP], LV_SCR_LOAD_ANIM_NONE);
    app_setup_open_panel(section);
}

/* -------------------------------------------------------------- launcher */

static void create_launcher(void)
{
    lv_obj_t *content;
    s_launcher = ui_plain_screen(&content);
    lv_obj_set_style_pad_all(content, 20, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(content, 20, 0);
    lv_obj_set_style_pad_column(content, 20, 0);

    lv_obj_t *title = lv_label_create(content);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_label_set_text(title, "Apps");
    lv_obj_set_width(title, LV_PCT(100));

    static const struct {
        const char *icon, *name;
        uint32_t color;
    } items[APP_COUNT] = {
        [APP_THERMO] = {UI_ICON_THERMO, "Thermostat", 0xD84315},
        [APP_HA] = {LV_SYMBOL_HOME, "Home Assistant", 0x1565C0},
        [APP_CALC] = {UI_ICON_CALC, "Calculator", 0x2E7D32},
        [APP_BLOCKS] = {UI_ICON_GAME, "Blocks", 0x6A1B9A},
        [APP_SETUP] = {LV_SYMBOL_SETTINGS, "Settings", 0x455A64},
    };
    for (int i = 0; i < APP_COUNT; i++) {
        lv_obj_t *b = lv_button_create(content);
        lv_obj_set_size(b, 210, 170);
        lv_obj_set_style_bg_color(b, ui_c(items[i].color), 0);
        lv_obj_set_style_radius(b, 16, 0);
        lv_obj_add_event_cb(b, open_app_cb, LV_EVENT_CLICKED, s_apps[i]);
        lv_obj_t *ic = lv_label_create(b);
        lv_obj_set_style_text_font(ic, &font_icons_48, 0);
        lv_label_set_text(ic, items[i].icon);
        lv_obj_align(ic, LV_ALIGN_CENTER, 0, -22);
        lv_obj_t *nm = lv_label_create(b);
        lv_label_set_text(nm, items[i].name);
        lv_obj_align(nm, LV_ALIGN_BOTTOM_MID, 0, -6);
    }
}

/* ---------------------------------------------------------- swipe guard */

// A tap counts only if the finger stayed close to where it touched down. A swipe that did not turn into a page
// scroll (too short, diagonal, ended on the same row) used to "click" whatever it started on. Settings: Display >
// Swipe protection. Drag controls (the dial, sliders) are left alone.
static const uint8_t GUARD_PX[4] = {0, 28, 18, 12};  // off, light, normal, strong
static lv_point_t s_press_pt;

static void swipe_guard_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (!in) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (code == LV_EVENT_PRESSED) {
        s_press_pt = p;
        return;
    }
    if (code != LV_EVENT_CLICKED && code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_RELEASED) {
        return;
    }
    uint8_t g = settings_get()->swipe_guard;
    int lim = g < 4 ? GUARD_PX[g] : 0;
    int dx = p.x - s_press_pt.x, dy = p.y - s_press_pt.y;
    lv_obj_t *obj = lv_event_get_param(e);
    if (!lim || !obj || dx * dx + dy * dy < lim * lim) {
        return;
    }
    if (lv_obj_check_type(obj, &lv_arc_class) || lv_obj_check_type(obj, &lv_slider_class)) {
        return;  // dragging is how these are used
    }
    if (code == LV_EVENT_RELEASED) {
        // Button groups and dropdowns act on release: drop the selection / the press instead
        if (lv_obj_check_type(obj, &lv_buttonmatrix_class) || lv_obj_check_type(obj, &lv_keyboard_class)) {
            lv_buttonmatrix_set_selected_button(obj, LV_BUTTONMATRIX_BUTTON_NONE);
        } else if (lv_obj_check_type(obj, &lv_dropdown_class)) {
            lv_obj_remove_state(obj, LV_STATE_PRESSED);
            lv_indev_stop_processing(in);
        }
        return;
    }
    lv_indev_stop_processing(in);  // the object never sees this click
}

// Test touch (console "GO touch x0 y0 x1 y1 ms"): a straight finger path from x0,y0 to x1,y1 in ms, then lift.
// Replaces the touch panel's readings while it runs.
static lv_indev_read_cb_t s_touch_read;
static volatile bool s_inj_on;
static int64_t s_inj_t0;
static int s_inj_x0, s_inj_y0, s_inj_x1, s_inj_y1, s_inj_ms;

static void touch_read_wrap(lv_indev_t *in, lv_indev_data_t *d)
{
    if (!s_inj_on) {
        s_touch_read(in, d);
        return;
    }
    int64_t el = (esp_timer_get_time() - s_inj_t0) / 1000;
    if (el <= s_inj_ms) {
        float f = s_inj_ms ? (float)el / (float)s_inj_ms : 1;
        d->point.x = s_inj_x0 + (int)((s_inj_x1 - s_inj_x0) * f);
        d->point.y = s_inj_y0 + (int)((s_inj_y1 - s_inj_y0) * f);
        d->state = LV_INDEV_STATE_PRESSED;
    } else {
        d->point.x = s_inj_x1;
        d->point.y = s_inj_y1;
        d->state = LV_INDEV_STATE_RELEASED;
        s_inj_on = false;
    }
}

static void swipe_guard_install(void)
{
    for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in)) {
        if (lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER) {
            lv_indev_add_event_cb(in, swipe_guard_cb, LV_EVENT_ALL, NULL);
            if (!s_touch_read) {
                s_touch_read = lv_indev_get_read_cb(in);
                lv_indev_set_read_cb(in, touch_read_wrap);
            }
        }
    }
}

void ui_shell_create(void)
{
    swipe_guard_install();
    s_apps[APP_THERMO] = app_thermo_create();
    s_apps[APP_HA] = app_ha_create();
    s_apps[APP_CALC] = app_calc_create();
    s_apps[APP_BLOCKS] = app_blocks_create();
    s_apps[APP_SETUP] = app_setup_create();
    create_launcher();
    quickpanel_create();  // before the status bar, which stays on top of it
    create_status_bar();
    lv_timer_create(idle_cb, 1000, NULL);
    lv_screen_load(s_apps[APP_THERMO]);
}

void ui_debug_go(const char *what)
{
    static const char *names[APP_COUNT] = {"thermo", "ha", "calc", "blocks", "setup"};
    if (strcmp(what, "launcher") == 0) {
        lv_screen_load(s_launcher);
        return;
    }
    if (strcmp(what, "quick") == 0) {
        quickpanel_open();
        return;
    }
    if (strncmp(what, "theme ", 6) == 0) {  // "theme auto|light|dark": sets the theme setting
        static settings_t c;
        settings_copy(&c);
        c.theme_mode = strcmp(what + 6, "light") == 0 ? 1 : strcmp(what + 6, "dark") == 0 ? 2
                       : strcmp(what + 6, "hours") == 0 ? 3 : 0;
        settings_save(&c);
        ui_theme_update();
        return;
    }
    if (strncmp(what, "panel ", 6) == 0) {
        lv_screen_load(s_apps[APP_SETUP]);
        app_setup_open_panel(atoi(what + 6));
        return;
    }
    if (strncmp(what, "edit ", 5) == 0 || strcmp(what, "advanced") == 0 || strcmp(what, "service") == 0 ||
        strncmp(what, "day ", 4) == 0 || strncmp(what, "group ", 6) == 0) {
        ui_go_thermostat();
        ui_pages_debug(what);
        return;
    }
    if (strncmp(what, "touch ", 6) == 0) {  // "touch x0 y0 x1 y1 ms": a test finger path
        int v[5];
        if (sscanf(what + 6, "%d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4]) == 5 && s_touch_read) {
            s_inj_x0 = v[0], s_inj_y0 = v[1], s_inj_x1 = v[2], s_inj_y1 = v[3], s_inj_ms = v[4];
            s_inj_t0 = esp_timer_get_time();
            s_inj_on = true;
        }
        return;
    }
    if (strncmp(what, "page ", 5) == 0) {
        lv_screen_load(s_apps[APP_THERMO]);
        app_thermo_show_page(atoi(what + 5));
        return;
    }
    for (int i = 0; i < APP_COUNT; i++) {
        if (strcmp(what, names[i]) == 0) {
            lv_screen_load(s_apps[i]);
        }
    }
}
