// Settings: the one list of everything that can be set. "Heating" rows open the thermostat's setting groups
// (ui_pages.c); "Device" rows open a section here (Wi-Fi, Home Assistant, Clock, Web access, Sensor and boiler,
// Display, Diagnostics). A section is an overlay panel; while one is open the header shows its name and Back.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_lvgl_port.h"
#include "esp_hosted.h"
#include "board.h"
#include "settings.h"
#include "ui.h"
#include "ui_theme.h"
#include "ha.h"
#include "camera.h"
#include "timekeep.h"
#include "thermostat.h"
#include "selftest.h"
#include "c6update.h"
#include "motion.h"
#include "cfgtext.h"
#include "ui_pages.h"

static const char *TAG = "setup";

#define MAX_APS 30

typedef enum { CMD_SCAN, CMD_TEST, CMD_C6_UPDATE } cmd_t;

static QueueHandle_t s_cmds;
static settings_t s_tmp;  // scratch copy for edits (LVGL task only)
static lv_obj_t *s_content, *s_kb;
static bool s_visible;
#define N_PANELS 7
#define PANEL_SHIFT 56  // sections are laid out below a title row that the header now replaces
static lv_obj_t *s_panels[N_PANELS], *s_open, *s_hdr_back, *s_hdr_title, *s_picker;
static void show_panel(lv_obj_t *p);

/* --------------------------------------------------------------- helpers */

static void send_cmd(cmd_t c)
{
    xQueueSend(s_cmds, &c, 0);
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, int w, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    ui_label_text(l, text);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    if (color) {
        lv_obj_set_style_text_color(l, ui_c(color), 0);
    }
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, uint32_t color, lv_event_cb_t cb, int x, int y, int w, int h)
{
    lv_obj_t *b = ui_button(parent, text, ui_c(color), cb, NULL);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    return b;
}

/* --------------------------------------------------------------- keyboard */

static void kb_hide(void)
{
    lv_keyboard_set_textarea(s_kb, NULL);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    ui_set_busy(false);
}

static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (lv_event_get_code(e) == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(s_kb, ta);
        lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_kb);
        ui_set_busy(true);
    } else if (lv_event_get_code(e) == LV_EVENT_DEFOCUSED) {
        kb_hide();
    }
}

static void kb_event_cb(lv_event_t *e)
{
    kb_hide();
}

static lv_obj_t *textarea(lv_obj_t *parent, const char *placeholder, int x, int y, int w)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_width(ta, w);
    lv_obj_set_pos(ta, x, y);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_DEFOCUSED, NULL);
    return ta;
}

/* ------------------------------------------------------------------ panels */

// Shows one section (the header gets its name and a Back button), or the Settings list for NULL.
static void show_panel(lv_obj_t *p)
{
    kb_hide();
    for (int k = 0; k < N_PANELS; k++) {
        lv_obj_add_flag(s_panels[k], LV_OBJ_FLAG_HIDDEN);
    }
    s_open = p;
    if (p) {
        lv_obj_remove_flag(p, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(p);
        lv_obj_send_event(p, LV_EVENT_REFRESH, NULL);
    }
    lv_label_set_text(s_hdr_title, p ? (const char *)lv_obj_get_user_data(p) : "Settings");
    lv_label_set_text(lv_obj_get_child(s_hdr_back, 0), p ? LV_SYMBOL_LEFT " Back" : LV_SYMBOL_LEFT " Apps");
}

static void panel_open_cb(lv_event_t *e)
{
    show_panel(lv_event_get_user_data(e));
}

// The header button: closes the network list, then the open section, then goes to the app menu.
static void hdr_back_cb(lv_event_t *e)
{
    if (!lv_obj_has_flag(s_picker, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
        ui_set_busy(false);
    } else if (s_open) {
        show_panel(NULL);
    } else {
        ui_go_launcher();
    }
}

// A section panel; its title is shown in the header while it is open.
static lv_obj_t *panel(const char *title)
{
    lv_obj_t *p = ui_overlay(s_content);
    lv_obj_set_user_data(p, (void *)title);
    return p;
}

/* ================================================================== Wi-Fi */

static wifi_ap_record_t s_aps[MAX_APS];
static char s_chosen_ssid[33];
static lv_obj_t *s_wifi_status, *s_list, *s_scan_lbl, *s_pw_panel, *s_pw_title, *s_pw_ta, *s_pw_kb;

static void wifi_item_cb(lv_event_t *e);

static void fill_list(int n)
{
    lv_obj_clean(s_list);
    if (n < 0) {
        ui_label_text(s_scan_lbl, "Scan failed - is the Wi-Fi chip OK? Tap Rescan.");
        return;
    }
    ui_label_fmt(s_scan_lbl, "%d networks found. Tap one.", n);
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)s_aps[i].ssid;
        if (!ssid[0]) {
            continue;
        }
        bool dup = false;  // same SSID from several access points: list once (strongest first)
        for (int j = 0; j < i; j++) {
            dup |= strcmp(ssid, (const char *)s_aps[j].ssid) == 0;
        }
        if (dup) {
            continue;
        }
        char text[64];
        snprintf(text, sizeof(text), "%s   %d dBm%s", ssid, s_aps[i].rssi,
                 s_aps[i].authmode == WIFI_AUTH_OPEN ? "  (open)" : "");
        lv_obj_t *b = lv_list_add_button(s_list, LV_SYMBOL_WIFI, text);
        lv_obj_set_height(b, 64);
        lv_obj_add_event_cb(b, wifi_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void open_picker(void)
{
    lv_obj_remove_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_picker);
    lv_obj_add_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(s_list);
    ui_label_text(s_scan_lbl, "Scanning...");
    send_cmd(CMD_SCAN);
}

static void choose_cb(lv_event_t *e)
{
    open_picker();
}

static void rescan_cb(lv_event_t *e)
{
    lv_obj_clean(s_list);
    ui_label_text(s_scan_lbl, "Scanning...");
    send_cmd(CMD_SCAN);
}

static void close_picker_cb(lv_event_t *e)
{
    lv_obj_add_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
    ui_set_busy(false);
}

static void wifi_item_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    strlcpy(s_chosen_ssid, (const char *)s_aps[i].ssid, sizeof(s_chosen_ssid));
    ui_label_fmt(s_pw_title, "Password for \"%s\"", s_chosen_ssid);
    lv_textarea_set_text(s_pw_ta, "");
    lv_obj_remove_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_pw_kb, s_pw_ta);
    ui_set_busy(true);
}

static void do_connect(void)
{
    board_wifi_connect(s_chosen_ssid, lv_textarea_get_text(s_pw_ta));
    lv_obj_add_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
    ui_set_busy(false);
}

static void connect_cb(lv_event_t *e)
{
    do_connect();
}

static void pw_kb_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        do_connect();
    } else {
        lv_obj_add_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void cancel_pw_cb(lv_event_t *e)
{
    lv_obj_add_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
}

static void show_pw_cb(lv_event_t *e)
{
    lv_obj_t *cb = lv_event_get_target(e);
    lv_textarea_set_password_mode(s_pw_ta, !lv_obj_has_state(cb, LV_STATE_CHECKED));
}

static lv_obj_t *build_wifi(void)
{
    lv_obj_t *p = panel("Wi-Fi");
    s_wifi_status = label(p, "", 16, 76, 448, 0x90A4AE);
    button(p, "Choose network", 0x1565C0, choose_cb, 16, 140, 448, 64);
    label(p, "Pick your network and type its password on the screen keyboard. "
             "The device reconnects on its own if the network drops.", 16, 224, 448, 0x607D8B);

    // Network list overlay (covers the whole Setup area)
    s_picker = ui_overlay(s_content);
    lv_obj_t *pt = lv_label_create(s_picker);
    lv_obj_set_style_text_font(pt, &lv_font_montserrat_28, 0);
    ui_label_text(pt, "Choose network");
    lv_obj_set_pos(pt, 16, 14);
    s_scan_lbl = label(s_picker, "", 16, 56, 0, 0x90A4AE);
    s_list = lv_list_create(s_picker);
    lv_obj_set_size(s_list, 448, 520);
    lv_obj_set_pos(s_list, 16, 88);
    lv_obj_t *rs = ui_button(s_picker, LV_SYMBOL_REFRESH " Rescan", ui_c(0x1565C0), rescan_cb, NULL);
    lv_obj_set_size(rs, 216, 64);
    lv_obj_set_pos(rs, 16, 624);
    lv_obj_t *cl = ui_button(s_picker, LV_SYMBOL_CLOSE " Close", ui_c(0x37474F), close_picker_cb, NULL);
    lv_obj_set_size(cl, 216, 64);
    lv_obj_set_pos(cl, 248, 624);

    // Password overlay (on top of the list)
    s_pw_panel = ui_overlay(s_picker);
    s_pw_title = label(s_pw_panel, "", 16, 16, 0, 0);
    lv_obj_set_width(s_pw_title, 448);
    lv_label_set_long_mode(s_pw_title, LV_LABEL_LONG_DOT);
    s_pw_ta = lv_textarea_create(s_pw_panel);
    lv_textarea_set_one_line(s_pw_ta, true);
    lv_textarea_set_password_mode(s_pw_ta, true);
    lv_textarea_set_max_length(s_pw_ta, 63);
    lv_textarea_set_placeholder_text(s_pw_ta, "Password (empty for open networks)");
    lv_obj_set_width(s_pw_ta, 448);
    lv_obj_set_pos(s_pw_ta, 16, 52);
    lv_obj_t *show = lv_checkbox_create(s_pw_panel);
    lv_checkbox_set_text(show, "Show password");
    lv_obj_set_pos(show, 16, 124);
    lv_obj_add_event_cb(show, show_pw_cb, LV_EVENT_VALUE_CHANGED, NULL);
    button(s_pw_panel, LV_SYMBOL_OK " Connect", 0x2E7D32, connect_cb, 16, 172, 216, 64);
    button(s_pw_panel, "Cancel", 0x37474F, cancel_pw_cb, 248, 172, 216, 64);
    s_pw_kb = lv_keyboard_create(s_pw_panel);
    lv_obj_set_size(s_pw_kb, LV_PCT(100), 320);
    lv_obj_align(s_pw_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_pw_kb, s_pw_ta);
    lv_obj_add_event_cb(s_pw_kb, pw_kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_pw_kb, pw_kb_cb, LV_EVENT_CANCEL, NULL);
    return p;
}

/* ========================================================= Home Assistant */

static lv_obj_t *s_ha_url, *s_ha_status;

static void ha_refresh(void)
{
    const settings_t *c = settings_get();
    const char *res = ha_last_result();
    char info[96] = "#FFB74D missing#";
    if (c->ha_token[0]) {
        ha_token_describe(c->ha_token, info, sizeof(info));
    }
    ui_label_fmt(s_ha_status, "Token: %s\nLast test: %s", info, res[0] ? res : "not run yet");
}

static void ha_panel_cb(lv_event_t *e)
{
    lv_textarea_set_text(s_ha_url, settings_get()->ha_url);
    ha_refresh();
}

static void ha_save_url_cb(lv_event_t *e)
{
    settings_copy(&s_tmp);
    char url[120];  // leaves room for "http://"
    strlcpy(url, lv_textarea_get_text(s_ha_url), sizeof(url));
    size_t n = strlen(url);
    if (n && url[n - 1] == '/') {
        url[n - 1] = 0;
    }
    if (url[0] && strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        snprintf(s_tmp.ha_url, sizeof(s_tmp.ha_url), "http://%s", url);
    } else {
        strlcpy(s_tmp.ha_url, url, sizeof(s_tmp.ha_url));
    }
    settings_save(&s_tmp);
    lv_textarea_set_text(s_ha_url, s_tmp.ha_url);
    kb_hide();
    send_cmd(CMD_TEST);
}

static void ha_test_cb(lv_event_t *e)
{
    ui_label_text(s_ha_status, "Testing...");
    send_cmd(CMD_TEST);
}

static lv_obj_t *build_ha(void)
{
    lv_obj_t *p = panel("Home Assistant");
    lv_obj_add_event_cb(p, ha_panel_cb, LV_EVENT_REFRESH, NULL);
    label(p, "Home Assistant URL", 16, 70, 0, 0);
    s_ha_url = textarea(p, "http://homeassistant.local:8123", 16, 98, 330);
    lv_textarea_set_max_length(s_ha_url, 120);
    button(p, "Save", 0x2E7D32, ha_save_url_cb, 356, 98, 108, 52);

    label(p, "The access token is entered on the web page (Settings > Web access shows its address): in HA, your profile > "
             "Security > Long-lived access tokens > Create token, then paste it there.",
          16, 176, 448, 0x90A4AE);
    s_ha_status = label(p, "", 16, 290, 448, 0);
    lv_label_set_recolor(s_ha_status, true);
    button(p, "Test connection", 0x37474F, ha_test_cb, 16, 370, 448, 60);
    return p;
}

/* ================================================================== Clock */

#define TZS TIMEKEEP_ZONES  // shared with the web page

static lv_obj_t *s_ntp_sw, *s_ntp_srv, *s_tz_dd, *s_now_lbl, *s_rl[5];

static void clock_panel_cb(lv_event_t *e)
{
    const settings_t *c = settings_get();
    lv_obj_set_state(s_ntp_sw, LV_STATE_CHECKED, c->ntp_on);
    lv_textarea_set_text(s_ntp_srv, c->ntp_server);
    for (size_t i = 0; i < (size_t)TIMEKEEP_ZONE_COUNT; i++) {
        if (strcmp(TZS[i].tz, c->tz) == 0) {
            lv_dropdown_set_selected(s_tz_dd, i);
        }
    }
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    if (timekeep_valid()) {
        lv_roller_set_selected(s_rl[0], lt.tm_year + 1900 - 2025, LV_ANIM_OFF);
        lv_roller_set_selected(s_rl[1], lt.tm_mon, LV_ANIM_OFF);
        lv_roller_set_selected(s_rl[2], lt.tm_mday - 1, LV_ANIM_OFF);
        lv_roller_set_selected(s_rl[3], lt.tm_hour, LV_ANIM_OFF);
        lv_roller_set_selected(s_rl[4], lt.tm_min, LV_ANIM_OFF);
    }
}

static void clock_save(void)
{
    settings_copy(&s_tmp);
    s_tmp.ntp_on = lv_obj_has_state(s_ntp_sw, LV_STATE_CHECKED);
    strlcpy(s_tmp.ntp_server, lv_textarea_get_text(s_ntp_srv), sizeof(s_tmp.ntp_server));
    strlcpy(s_tmp.tz, TZS[lv_dropdown_get_selected(s_tz_dd)].tz, sizeof(s_tmp.tz));
    settings_save(&s_tmp);
    timekeep_apply_settings();
}

static void clock_changed_cb(lv_event_t *e)
{
    clock_save();
}

static void ntp_srv_ready_cb(lv_event_t *e)
{
    clock_save();
}

static void set_clock_cb(lv_event_t *e)
{
    // Setting the time by hand turns NTP off, so the next sync does not overwrite it.
    lv_obj_remove_state(s_ntp_sw, LV_STATE_CHECKED);
    clock_save();
    struct tm t = {
        .tm_year = 2025 + lv_roller_get_selected(s_rl[0]) - 1900,
        .tm_mon = lv_roller_get_selected(s_rl[1]),
        .tm_mday = lv_roller_get_selected(s_rl[2]) + 1,
        .tm_hour = lv_roller_get_selected(s_rl[3]),
        .tm_min = lv_roller_get_selected(s_rl[4]),
    };
    timekeep_set_manual(&t);
}

static void clock_tick_cb(lv_timer_t *t)
{
    if (!s_visible) {
        return;
    }
    char buf[64] = "Clock not set";
    if (timekeep_valid()) {
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        strftime(buf, sizeof(buf), "%Y-%m-%d  %H:%M:%S", &lt);
    }
    ui_label_fmt(s_now_lbl, "%s  (%s)", buf, timekeep_source_name());
}

static char *num_list(int from, int to, int width)
{
    static char buf[6][400];
    static int k;
    char *b = buf[k++ % 6];
    b[0] = 0;
    for (int i = from; i <= to; i++) {
        char n[8];
        snprintf(n, sizeof(n), "%0*d%s", width, i, i < to ? "\n" : "");
        strlcat(b, n, 400);
    }
    return b;
}

static lv_obj_t *build_clock(void)
{
    lv_obj_t *p = panel("Clock");
    lv_obj_add_event_cb(p, clock_panel_cb, LV_EVENT_REFRESH, NULL);
    s_now_lbl = label(p, "", 16, 70, 448, 0x4FC3F7);

    label(p, "Internet time (NTP)", 16, 110, 0, 0);
    s_ntp_sw = lv_switch_create(p);
    lv_obj_set_pos(s_ntp_sw, 390, 104);
    lv_obj_add_event_cb(s_ntp_sw, clock_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    label(p, "NTP server", 16, 152, 0, 0x90A4AE);
    s_ntp_srv = textarea(p, "pool.ntp.org", 140, 140, 324);
    lv_obj_add_event_cb(s_ntp_srv, ntp_srv_ready_cb, LV_EVENT_READY, NULL);
    label(p, "Time zone", 16, 210, 0, 0x90A4AE);
    s_tz_dd = lv_dropdown_create(p);
    char opts[300] = "";
    for (size_t i = 0; i < (size_t)TIMEKEEP_ZONE_COUNT; i++) {
        strlcat(opts, TZS[i].name, sizeof(opts));
        if (i + 1 < (size_t)TIMEKEEP_ZONE_COUNT) {
            strlcat(opts, "\n", sizeof(opts));
        }
    }
    lv_dropdown_set_options(s_tz_dd, opts);
    lv_obj_set_width(s_tz_dd, 324);
    lv_obj_set_pos(s_tz_dd, 140, 200);
    lv_obj_add_event_cb(s_tz_dd, clock_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    label(p, "Set by hand (turns NTP off)", 16, 270, 0, 0);
    static const int w[5] = {100, 72, 72, 72, 72};
    static const char *const unit[5] = {"year", "month", "day", "hour", "min"};
    const char *opt[5] = {num_list(2025, 2040, 4), num_list(1, 12, 2), num_list(1, 31, 2), num_list(0, 23, 2),
                          num_list(0, 59, 2)};
    int x = 16;
    for (int i = 0; i < 5; i++) {
        s_rl[i] = lv_roller_create(p);
        lv_roller_set_options(s_rl[i], opt[i], LV_ROLLER_MODE_NORMAL);
        lv_roller_set_visible_row_count(s_rl[i], 3);
        lv_obj_set_width(s_rl[i], w[i]);
        lv_obj_set_pos(s_rl[i], x, 302);
        lv_obj_t *u = label(p, unit[i], x, 438, 0, 0x607D8B);  // centred under its column
        lv_obj_set_width(u, w[i]);
        lv_obj_set_style_text_align(u, LV_TEXT_ALIGN_CENTER, 0);
        x += w[i] + 12;
    }
    button(p, "Set clock", 0x1565C0, set_clock_cb, 16, 470, 448, 64);
    label(p, "This board has no battery clock: a hand-set time is lost when power is cut. "
             "With Wi-Fi, NTP keeps the time correct.",
          16, 550, 448, 0x607D8B);
    lv_timer_create(clock_tick_cb, 1000, NULL);
    return p;
}

/* ============================================================= Web access */

static lv_obj_t *s_web_user, *s_web_pw, *s_web_msg, *s_qr, *s_qr_lbl;
static char s_qr_ip[16] = "-";

static void web_panel_cb(lv_event_t *e)
{
    lv_textarea_set_text(s_web_user, settings_get()->web_user);
    lv_textarea_set_text(s_web_pw, "");
    ui_label_text(s_web_msg, settings_web_configured() ? "Login is set. Enter a new password to change it."
                                                           : "#FFB74D Not set: the web page is locked until you set a password.#");
}

static void web_save_cb(lv_event_t *e)
{
    const char *user = lv_textarea_get_text(s_web_user);
    const char *pw = lv_textarea_get_text(s_web_pw);
    if (!user[0] || strlen(pw) < 8) {
        ui_label_text(s_web_msg, "#E57373 Username and a password of at least 8 characters, please.#");
        return;
    }
    settings_copy(&s_tmp);
    settings_set_web_password(&s_tmp, user, pw);
    settings_save(&s_tmp);
    lv_textarea_set_text(s_web_pw, "");
    kb_hide();
    ui_label_fmt(s_web_msg, "#81C784 " LV_SYMBOL_OK " Saved. Log in on the web page as \"%s\".#", user);
}

static lv_obj_t *build_web(void)
{
    lv_obj_t *p = panel("Web access");
    lv_obj_add_event_cb(p, web_panel_cb, LV_EVENT_REFRESH, NULL);
    label(p, "The web page (address below) asks for this login.", 16, 76, 448, 0x90A4AE);
    label(p, "Username", 16, 136, 0, 0);
    s_web_user = textarea(p, "admin", 140, 124, 324);
    lv_textarea_set_max_length(s_web_user, 32);
    label(p, "Password", 16, 200, 0, 0);
    s_web_pw = textarea(p, "at least 8 characters", 140, 188, 324);
    lv_textarea_set_password_mode(s_web_pw, true);
    lv_textarea_set_max_length(s_web_pw, 64);
    button(p, "Save login", 0x2E7D32, web_save_cb, 16, 256, 448, 60);
    s_web_msg = label(p, "", 16, 330, 448, 0);
    lv_label_set_recolor(s_web_msg, true);
    // The page's address, also as a QR code
    s_qr = lv_qrcode_create(p);
    lv_qrcode_set_size(s_qr, 150);
    lv_qrcode_set_dark_color(s_qr, lv_color_black());
    lv_qrcode_set_light_color(s_qr, lv_color_white());
    lv_obj_set_style_border_color(s_qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_qr, 6, 0);
    lv_obj_set_pos(s_qr, 16, 410);
    lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
    s_qr_lbl = label(p, "", 186, 414, 280, 0);
    lv_label_set_recolor(s_qr_lbl, true);
    return p;
}

/* ============================================================= Thermostat */

static lv_obj_t *s_ofs_lbl, *s_sim_sw, *s_ot_lbl;

static void thermo_refresh(void)
{
    thermo_state_t st;
    thermo_get(&st);
    ui_label_fmt(s_ofs_lbl, "%+.1f °C", st.offset);
    lv_obj_set_state(s_sim_sw, LV_STATE_CHECKED, st.boiler_sim);
    ui_label_fmt(s_ot_lbl,
                          "Boiler: %s%s\nOpenTherm frames: %lu ok, %lu failed\nRoom reading: %s (%lu sensor errors)\n"
                          "Control loop runs: %lu",
                          st.boiler_sim ? "simulated" : "real", st.boiler_online ? ", answering" : ", not answering",
                          (unsigned long)st.ot_ok, (unsigned long)st.ot_errors, st.sensor_ok ? "OK" : "missing",
                          (unsigned long)st.sensor_errors, (unsigned long)st.loops);
}

static void thermo_panel_cb(lv_event_t *e)
{
    thermo_refresh();
}

static void ofs_cb(lv_event_t *e)
{
    thermo_state_t st;
    thermo_get(&st);
    thermo_set_offset(st.offset + (int)(intptr_t)lv_event_get_user_data(e) / 10.0f);
    thermo_refresh();
}

static void sim_cb(lv_event_t *e)
{
    thermo_set_boiler_sim(lv_obj_has_state(s_sim_sw, LV_STATE_CHECKED));
    thermo_refresh();
}

static void thermo_tick_cb(lv_timer_t *t)
{
    if (s_visible) {
        thermo_refresh();
    }
}

static void stepper(lv_obj_t *p, const char *name, const char *hint, int y, lv_obj_t **val, lv_event_cb_t cb, int step)
{
    label(p, name, 16, y, 0, 0);
    label(p, hint, 16, y + 28, 200, 0x607D8B);
    lv_obj_t *m = ui_button(p, LV_SYMBOL_MINUS, ui_c(0x1565C0), cb, (void *)(intptr_t)-step);
    lv_obj_set_size(m, 70, 56);
    lv_obj_set_pos(m, 224, y);
    *val = lv_label_create(p);
    lv_obj_set_width(*val, 100);
    lv_obj_set_style_text_align(*val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(*val, 294, y + 16);
    lv_obj_t *pl = ui_button(p, LV_SYMBOL_PLUS, ui_c(0xD84315), cb, (void *)(intptr_t)step);
    lv_obj_set_size(pl, 70, 56);
    lv_obj_set_pos(pl, 394, y);
}

static lv_obj_t *build_thermo(void)
{
    lv_obj_t *p = panel("Sensor and boiler");
    lv_obj_add_event_cb(p, thermo_panel_cb, LV_EVENT_REFRESH, NULL);
    stepper(p, "Sensor correction", "added to the measured room temperature", 76, &s_ofs_lbl, ofs_cb, 1);
    label(p, "Simulated boiler", 16, 186, 0, 0);
    label(p, "On: a built-in model of a boiler and a room, for trying things out. Off: the real boiler, over "
             "OpenTherm.", 16, 214, 350, 0x607D8B);
    s_sim_sw = lv_switch_create(p);
    lv_obj_set_pos(s_sim_sw, 390, 184);
    lv_obj_add_event_cb(s_sim_sw, sim_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_ot_lbl = label(p, "", 16, 320, 448, 0x90A4AE);
    lv_timer_create(thermo_tick_cb, 1000, NULL);
    return p;
}

/* ================================================================ Display */

static lv_obj_t *s_br, *s_dim, *s_dim_dd, *s_off_dd, *s_home_dd, *s_keep_sw, *s_k_st, *s_k_en;
static lv_obj_t *s_night_sw, *s_n_st, *s_n_en, *s_n_lvl, *s_mot_sw, *s_mot_dd, *s_mot_lbl, *s_theme_dd, *s_d_st, *s_d_en;
static lv_obj_t *s_swipe_dd;
static const uint16_t DIM_S[] = {0, 15, 30, 60, 120, 300, 600};
static const char DIM_OPTS[] = "Never\n15 s\n30 s\n1 min\n2 min\n5 min\n10 min";
static const uint16_t OFF_S[] = {0, 60, 120, 300, 600, 1800, 3600};
static const char OFF_OPTS[] = "Never\n1 min\n2 min\n5 min\n10 min\n30 min\n1 hour";
static const uint16_t HOME_S[] = {0, 30, 60, 120, 300};
static const char HOME_OPTS[] = "Never\n30 s\n1 min\n2 min\n5 min";

static int index_of(const uint16_t *a, int n, uint16_t v)
{
    for (int i = 0; i < n; i++) {
        if (a[i] == v) {
            return i;
        }
    }
    return 0;
}

static void display_panel_cb(lv_event_t *e)
{
    const settings_t *c = settings_get();
    lv_slider_set_value(s_br, c->bright, LV_ANIM_OFF);
    lv_slider_set_value(s_dim, c->dim_level, LV_ANIM_OFF);
    lv_dropdown_set_selected(s_dim_dd, index_of(DIM_S, 7, c->dim_after_s));
    lv_dropdown_set_selected(s_off_dd, index_of(OFF_S, 7, c->off_after_s));
    lv_dropdown_set_selected(s_home_dd, index_of(HOME_S, 5, c->home_after_s));
    lv_obj_set_state(s_keep_sw, LV_STATE_CHECKED, c->keep_on);
    lv_dropdown_set_selected(s_k_st, c->keep_on_start / 30);
    lv_dropdown_set_selected(s_k_en, c->keep_on_end / 30);
    lv_obj_set_state(s_night_sw, LV_STATE_CHECKED, c->night_on);
    lv_dropdown_set_selected(s_n_st, c->night_start / 30);
    lv_dropdown_set_selected(s_n_en, c->night_end / 30);
    lv_slider_set_value(s_n_lvl, c->night_level, LV_ANIM_OFF);
    lv_obj_set_state(s_mot_sw, LV_STATE_CHECKED, c->motion_wake);
    lv_dropdown_set_selected(s_mot_dd, c->motion_sens);
    lv_dropdown_set_selected(s_theme_dd, c->theme_mode);
    lv_dropdown_set_selected(s_d_st, c->day_start / 30);
    lv_dropdown_set_selected(s_d_en, c->day_end / 30);
    lv_dropdown_set_selected(s_swipe_dd, c->swipe_guard);
}

static void display_save_cb(lv_event_t *e)
{
    settings_copy(&s_tmp);
    s_tmp.bright = lv_slider_get_value(s_br);
    s_tmp.dim_level = lv_slider_get_value(s_dim);
    s_tmp.dim_after_s = DIM_S[lv_dropdown_get_selected(s_dim_dd)];
    s_tmp.off_after_s = OFF_S[lv_dropdown_get_selected(s_off_dd)];
    s_tmp.home_after_s = HOME_S[lv_dropdown_get_selected(s_home_dd)];
    s_tmp.keep_on = lv_obj_has_state(s_keep_sw, LV_STATE_CHECKED);
    s_tmp.keep_on_start = lv_dropdown_get_selected(s_k_st) * 30;
    s_tmp.keep_on_end = lv_dropdown_get_selected(s_k_en) * 30;
    s_tmp.night_on = lv_obj_has_state(s_night_sw, LV_STATE_CHECKED);
    s_tmp.night_start = lv_dropdown_get_selected(s_n_st) * 30;
    s_tmp.night_end = lv_dropdown_get_selected(s_n_en) * 30;
    s_tmp.night_level = lv_slider_get_value(s_n_lvl);
    s_tmp.motion_wake = lv_obj_has_state(s_mot_sw, LV_STATE_CHECKED);
    s_tmp.motion_sens = lv_dropdown_get_selected(s_mot_dd);
    s_tmp.theme_mode = lv_dropdown_get_selected(s_theme_dd);
    s_tmp.day_start = lv_dropdown_get_selected(s_d_st) * 30;
    s_tmp.day_end = lv_dropdown_get_selected(s_d_en) * 30;
    s_tmp.swipe_guard = lv_dropdown_get_selected(s_swipe_dd);
    settings_save(&s_tmp);
    ui_theme_update();
}

static void bright_preview_cb(lv_event_t *e)
{
    board_backlight(lv_slider_get_value(lv_event_get_target(e)));
}

static void motion_tick_cb(lv_timer_t *t)
{
    if (!s_visible) {
        return;
    }
    int ago = motion_seconds_since();
    char last[32] = "none yet";
    if (ago >= 0) {
        snprintf(last, sizeof(last), "%d s ago", ago);
    }
    ui_label_fmt(s_mot_lbl, "Camera: %s. Last motion: %s (%lu total).", motion_state_name(), last,
                          (unsigned long)motion_count());
}

static lv_obj_t *slider(lv_obj_t *p, const char *name, int y, int min)
{
    label(p, name, 16, y, 0, 0);
    lv_obj_t *s = lv_slider_create(p);
    lv_slider_set_range(s, min, 100);
    lv_obj_set_width(s, 220);
    lv_obj_set_pos(s, 230, y + 6);
    lv_obj_add_event_cb(s, bright_preview_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, display_save_cb, LV_EVENT_RELEASED, NULL);
    return s;
}

static lv_obj_t *dropdown(lv_obj_t *p, const char *name, const char *opts, int x, int y, int w)
{
    if (name) {
        label(p, name, 16, y + 12, 0, 0);
    }
    lv_obj_t *d = lv_dropdown_create(p);
    lv_dropdown_set_options(d, opts);
    lv_obj_set_width(d, w);
    lv_obj_set_pos(d, x, y);
    lv_obj_add_event_cb(d, display_save_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return d;
}

static lv_obj_t *toggle(lv_obj_t *p, const char *name, int y)
{
    label(p, name, 16, y + 6, 0, 0);
    lv_obj_t *sw = lv_switch_create(p);
    lv_obj_set_pos(sw, 390, y);
    lv_obj_add_event_cb(sw, display_save_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static void section(lv_obj_t *p, const char *text, int y)
{
    lv_obj_t *l = label(p, text, 16, y, 448, 0x4FC3F7);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
}

static lv_obj_t *build_display(void)
{
    static char halfhours[48 * 6];
    halfhours[0] = 0;
    for (int i = 0; i < 48; i++) {
        char t[8];
        snprintf(t, sizeof(t), "%02d:%02d%s", i / 2, (i % 2) * 30, i < 47 ? "\n" : "");
        strlcat(halfhours, t, sizeof(halfhours));
    }
    lv_obj_t *p = panel("Display");
    lv_obj_add_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_add_event_cb(p, display_panel_cb, LV_EVENT_REFRESH, NULL);

    s_br = slider(p, "Brightness", 80, 5);
    section(p, "When nobody touches the screen", 126);
    s_dim_dd = dropdown(p, "Dim after", DIM_OPTS, 230, 156, 220);
    s_dim = slider(p, "Dimmed level", 218, 1);
    s_off_dd = dropdown(p, "Screen off after", OFF_OPTS, 230, 256, 220);
    s_home_dd = dropdown(p, "Back to thermostat", HOME_OPTS, 230, 316, 220);

    section(p, "Keep the screen on (schedule)", 386);
    s_keep_sw = toggle(p, "Never turn off between", 416);
    label(p, "from", 16, 478, 0, 0x90A4AE);
    s_k_st = dropdown(p, NULL, halfhours, 80, 466, 140);
    label(p, "until", 240, 478, 0, 0x90A4AE);
    s_k_en = dropdown(p, NULL, halfhours, 310, 466, 140);
    label(p, "Outside these hours the screen turns off after the time set above.", 16, 522, 448, 0x607D8B);

    section(p, "Night brightness", 586);
    s_night_sw = toggle(p, "Lower brightness at night", 616);
    label(p, "from", 16, 678, 0, 0x90A4AE);
    s_n_st = dropdown(p, NULL, halfhours, 80, 666, 140);
    label(p, "until", 240, 678, 0, 0x90A4AE);
    s_n_en = dropdown(p, NULL, halfhours, 310, 666, 140);
    s_n_lvl = slider(p, "Night brightness", 726, 1);

    section(p, "Wake on motion (camera)", 786);
    s_mot_sw = toggle(p, "Turn the screen on when someone moves in front of it", 816);
    lv_obj_set_width(lv_obj_get_child(p, lv_obj_get_child_count(p) - 2), 360);
    s_mot_dd = dropdown(p, "Sensitivity", "Low\nMedium\nHigh", 230, 870, 220);
    s_mot_lbl = label(p, "", 16, 930, 448, 0x90A4AE);
    label(p, "Works in a lit room (no infrared: in the dark only touch wakes it). Images stay on the device. "
             "The camera adds a little heat: re-check the sensor offset after enabling.",
          16, 990, 448, 0x607D8B);
    label(p, "The first touch on a dimmed or dark screen only wakes it up.", 16, 1080, 448, 0x607D8B);

    section(p, "Theme (day / night)", 1140);
    s_theme_dd = dropdown(p, "Theme", "Sunset (HA)\nLight\nDark\nBy the hours", 230, 1170, 220);
    label(p, "Day (light) from", 16, 1242, 0, 0x90A4AE);
    s_d_st = dropdown(p, NULL, halfhours, 230, 1230, 220);
    label(p, "until", 16, 1302, 0, 0x90A4AE);
    s_d_en = dropdown(p, NULL, halfhours, 230, 1290, 220);
    label(p, "Sunset (HA): light while the sun is up (Home Assistant sun.sun; without HA these hours). "
             "By the hours: light between these times. "
             "Also in the quick panel: tap the status bar.",
          16, 1350, 448, 0x607D8B);

    section(p, "Touch", 1450);
    s_swipe_dd = dropdown(p, "Swipe protection", "Off\nLight\nNormal\nStrong", 230, 1480, 220);
    label(p, "A tap only counts if the finger stays put: a swipe that starts on a button or a setting no longer "
             "opens it. Stronger = a smaller movement already cancels the tap.",
          16, 1550, 448, 0x607D8B);
    lv_timer_create(motion_tick_cb, 1000, NULL);
    return p;
}

/* ============================================================ Diagnostics */

static lv_obj_t *s_diag_lbl;

static void diag_panel_cb(lv_event_t *e)
{
    static char buf[1600];
    size_t n = 0;
    const selftest_item_t *it;
    int cnt = selftest_items(&it);
    static const char *col[] = {"90A4AE", "81C784", "FFB74D", "E57373"};
    thermo_state_t st;
    thermo_get(&st);
    for (int i = 0; i < cnt; i++) {
        // No boiler on the OpenTherm input is expected while the simulated boiler is in use: a note, not a warning
        if (it[i].result == ST_WARN && st.boiler_sim && strstr(it[i].name, "OpenTherm")) {
            n += snprintf(buf + n, sizeof(buf) - n, "#%s " LV_SYMBOL_MINUS "# %s\n    no boiler connected; the simulated boiler is in use\n",
                          col[0], it[i].name);
            continue;
        }
        n += snprintf(buf + n, sizeof(buf) - n, "#%s %s# %s\n    %s\n", col[it[i].result],
                      it[i].result == ST_PASS ? LV_SYMBOL_OK : it[i].result == ST_FAIL ? LV_SYMBOL_CLOSE : LV_SYMBOL_WARNING,
                      it[i].name, it[i].detail);
    }
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    n += snprintf(buf + n, sizeof(buf) - n,
                  "\nFirmware %s (%s %s), partition %s\nIP %s\nFree heap: %u KB internal, %u KB PSRAM\nUptime %lld min",
                  app->version, app->date, app->time, run ? run->label : "?", board_wifi_ip()[0] ? board_wifi_ip() : "-",
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), esp_timer_get_time() / 60000000);
    ui_label_text(s_diag_lbl, buf);
}

static void restart_cb(lv_event_t *e)
{
    esp_restart();
}

static lv_obj_t *s_c6_box, *s_c6_msg;

static void c6_close_cb(lv_event_t *e)
{
    lv_msgbox_close(s_c6_box);
    s_c6_box = NULL;
}

static void c6_go_cb(lv_event_t *e)
{
    lv_obj_t *footer = lv_msgbox_get_footer(s_c6_box);
    if (footer) {
        lv_obj_add_flag(footer, LV_OBJ_FLAG_HIDDEN);  // no way back once it runs
    }
    ui_label_text(s_c6_msg, "Updating... keep the power on. The device restarts when done.");
    ui_set_busy(true);
    send_cmd(CMD_C6_UPDATE);
}

static void c6_progress(int pct, void *user)
{
    lvgl_port_lock(0);
    if (s_c6_msg) {
        ui_label_fmt(s_c6_msg, "Updating the Wi-Fi chip: %d %%\nKeep the power on.", pct);
    }
    lvgl_port_unlock();
}

static void c6_update_task_part(void)
{
    esp_err_t err = c6update_run(c6_progress, NULL);  // restarts on success
    lvgl_port_lock(0);
    if (s_c6_msg) {
        ui_label_fmt(s_c6_msg, "Update failed: %s.\nThe Wi-Fi chip keeps its old firmware.", esp_err_to_name(err));
        lv_obj_t *footer = lv_msgbox_get_footer(s_c6_box);
        if (footer) {
            lv_obj_remove_flag(footer, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lvgl_port_unlock();
    ui_set_busy(false);
}

static void c6_cb(lv_event_t *e)
{
    uint32_t len = 0;
    if (s_c6_box) {
        return;
    }
    s_c6_box = lv_msgbox_create(NULL);
    lv_msgbox_add_title(s_c6_box, "Update Wi-Fi chip");
    if (!c6update_image_info(&len)) {
        s_c6_msg = lv_msgbox_add_text(s_c6_box, "No C6 firmware image stored on this device.\n"
                                                "Flash one with tools/flash_c6fw.sh first.");
        lv_obj_add_event_cb(lv_msgbox_add_footer_button(s_c6_box, "Close"), c6_close_cb, LV_EVENT_CLICKED, NULL);
        return;
    }
    static char txt[200];
    snprintf(txt, sizeof(txt), "Install ESP-Hosted %d.%d.%d on the ESP32-C6 (%lu KB)?\n"
                               "Takes about a minute; Wi-Fi is down meanwhile. The thermostat keeps running.",
             ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1, (unsigned long)(len / 1024));
    s_c6_msg = lv_msgbox_add_text(s_c6_box, txt);
    lv_obj_t *go = lv_msgbox_add_footer_button(s_c6_box, "Update");
    lv_obj_set_style_bg_color(go, ui_c(0xC62828), 0);
    lv_obj_add_event_cb(go, c6_go_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(lv_msgbox_add_footer_button(s_c6_box, "Cancel"), c6_close_cb, LV_EVENT_CLICKED, NULL);
}

static lv_obj_t *build_diag(void)
{
    lv_obj_t *p = panel("Diagnostics");
    lv_obj_add_event_cb(p, diag_panel_cb, LV_EVENT_REFRESH, NULL);
    lv_obj_t *box = lv_obj_create(p);
    lv_obj_set_size(box, 456, 524);
    lv_obj_set_pos(box, 12, 68);
    lv_obj_set_style_bg_color(box, ui_c(0x0D141B), 0);
    s_diag_lbl = lv_label_create(box);
    lv_label_set_recolor(s_diag_lbl, true);
    lv_obj_set_width(s_diag_lbl, 420);
    lv_label_set_long_mode(s_diag_lbl, LV_LABEL_LONG_WRAP);
    button(p, LV_SYMBOL_UPLOAD " Update Wi-Fi chip", 0x37474F, c6_cb, 16, 604, 448, 60);
    button(p, LV_SYMBOL_REFRESH " Restart device", 0x6D4C41, restart_cb, 16, 676, 448, 60);
    return p;
}

/* ============================================================ main screen */

static void update_cb(lv_timer_t *t)
{
    if (!s_visible) {
        return;
    }
    ui_label_text(s_wifi_status, board_wifi_status());
    const char *ip = board_wifi_ip();
    bool up = board_wifi_connected() && ip[0];
    if (strcmp(ip, s_qr_ip) != 0) {
        strlcpy(s_qr_ip, ip, sizeof(s_qr_ip));
        if (up) {
            char url[40];
            snprintf(url, sizeof(url), "https://%s/", ip);
            lv_qrcode_update(s_qr, url, strlen(url));
            lv_obj_remove_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (up) {
        ui_label_fmt(s_qr_lbl, "Web page\n#4FC3F7 https://%s/#\n%s", ip,
                              settings_web_configured() ? "Log in with the login above."
                                                        : "#FFB74D Set a login above first.#");
    } else {
        ui_label_text(s_qr_lbl, "Web page\nConnect to Wi-Fi first; the address and its QR code appear here.");
    }
}

static void worker(void *arg)
{
    cmd_t cmd;
    for (;;) {
        xQueueReceive(s_cmds, &cmd, portMAX_DELAY);
        if (cmd == CMD_SCAN) {
            int n = board_wifi_scan(s_aps, MAX_APS);
            lvgl_port_lock(0);
            fill_list(n);
            lvgl_port_unlock();
        } else if (cmd == CMD_TEST) {
            char msg[128];
            ha_test(msg, sizeof(msg));
            lvgl_port_lock(0);
            ha_refresh();
            lvgl_port_unlock();
        } else if (cmd == CMD_C6_UPDATE) {
            c6_update_task_part();
        }
    }
}

void app_setup_open_panel(int i)
{
    show_panel(i >= 0 && i < N_PANELS ? s_panels[i] : NULL);
}

static void screen_cb(lv_event_t *e)
{
    s_visible = lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED;
    if (s_visible) {
        s_qr_ip[0] = '-';  // redraw
        update_cb(NULL);
        if (!settings_wifi_configured()) {
            open_picker();
        }
    } else {
        kb_hide();
    }
}

static void group_cb(lv_event_t *e)
{
    ui_pages_open_group((int)(intptr_t)lv_event_get_user_data(e));
}

static void heading(lv_obj_t *list, const char *text)
{
    lv_obj_t *l = lv_label_create(list);
    lv_obj_set_style_text_color(l, ui_c(UI_VALUE), 0);
    lv_label_set_text(l, text);
}

// A row of the Settings list: icon, title, one line of what is inside, arrow.
static void row(lv_obj_t *list, const char *icon, const char *title, const char *brief, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(list);
    lv_obj_set_size(b, 440, 66);
    lv_obj_set_style_bg_color(b, ui_c(UI_SURFACE), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *ic = lv_label_create(b);
    lv_obj_set_style_text_color(ic, ui_c(UI_MUTED), 0);
    lv_obj_set_width(ic, 30);
    lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(ic, icon);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *n = lv_label_create(b);
    lv_obj_set_style_text_color(n, ui_c(UI_TEXT), 0);
    lv_label_set_text(n, title);
    lv_obj_set_pos(n, 52, 9);
    lv_obj_t *d = lv_label_create(b);
    lv_obj_set_style_text_font(d, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(d, ui_c(UI_MUTED), 0);
    lv_obj_set_size(d, 350, 20);  // one line, cut with dots
    lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);
    lv_label_set_text(d, brief);
    lv_obj_set_pos(d, 52, 38);
    lv_obj_t *a = lv_label_create(b);
    lv_obj_set_style_text_color(a, ui_c(UI_MUTED), 0);
    lv_label_set_text(a, LV_SYMBOL_RIGHT);
    lv_obj_align(a, LV_ALIGN_RIGHT_MID, -12, 0);
}

lv_obj_t *app_setup_create(void)
{
    lv_obj_t *scr = ui_app_screen("Settings", &s_content);
    lv_obj_add_event_cb(scr, screen_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(scr, screen_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);
    // The header's button and title follow the open section (show_panel)
    lv_obj_t *header = lv_obj_get_child(scr, 0);
    s_hdr_back = lv_obj_get_child(header, 0);
    s_hdr_title = lv_obj_get_child(header, 1);
    for (int i = (int)lv_obj_get_event_count(s_hdr_back) - 1; i >= 0; i--) {
        lv_obj_remove_event(s_hdr_back, (uint32_t)i);
    }
    lv_obj_add_event_cb(s_hdr_back, hdr_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *list = lv_obj_create(s_content);
    lv_obj_set_size(list, 480, UI_CONTENT_H);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_hor(list, 20, 0);
    lv_obj_set_style_pad_ver(list, 12, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);

    heading(list, "Heating");
    static const char *const gicon[CG_COUNT] = {
        [CG_PROGRAM] = LV_SYMBOL_LIST,   [CG_SUMMER] = UI_ICON_SUN,        [CG_DHW] = UI_ICON_DROP,
        [CG_LEGIONELLA] = LV_SYMBOL_REFRESH, [CG_SAFETY] = LV_SYMBOL_WARNING, [CG_SYSTEM] = UI_ICON_FIRE,
        [CG_TUNING] = LV_SYMBOL_SETTINGS, [CG_ALERTS] = LV_SYMBOL_BELL,     [CG_SERVICE] = LV_SYMBOL_PLAY,
    };
    for (int g = 0; g < CG_COUNT; g++) {
        row(list, gicon[g], cfgtext_group_title(g), cfgtext_group_brief(g), group_cb, (void *)(intptr_t)g);
    }

    heading(list, "Device");
    static const struct {
        const char *icon, *title, *brief;
        lv_obj_t *(*build)(void);
    } sections[N_PANELS] = {
        {LV_SYMBOL_WIFI, "Wi-Fi", "Network and password", build_wifi},
        {LV_SYMBOL_HOME, "Home Assistant", "Address, connection test", build_ha},
        {UI_ICON_CLOCK, "Clock", "Internet time, time zone, set by hand", build_clock},
        {UI_ICON_GLOBE, "Web access", "Address and login of the web page", build_web},
        {UI_ICON_THERMO, "Sensor and boiler", "Sensor correction, real or simulated boiler", build_thermo},
        {UI_ICON_DISPLAY, "Display", "Brightness, dimming, theme, wake on motion", build_display},
        {UI_ICON_DIAG, "Diagnostics", "Hardware check, versions, restart", build_diag},
    };
    for (int i = 0; i < N_PANELS; i++) {
        lv_obj_t *p = sections[i].build();
        s_panels[i] = p;
        for (uint32_t k = 0; k < lv_obj_get_child_count(p); k++) {  // up, into the space of the old title row
            lv_obj_t *ch = lv_obj_get_child(p, k);
            lv_obj_set_y(ch, lv_obj_get_y_aligned(ch) - PANEL_SHIFT);
        }
        row(list, sections[i].icon, sections[i].title, sections[i].brief, panel_open_cb, p);
    }
    // Panels are brought to the front when opened (show_panel), so creation order does not matter.

    s_kb = lv_keyboard_create(s_content);
    lv_obj_set_size(s_kb, LV_PCT(100), 300);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_CANCEL, NULL);

    s_cmds = xQueueCreate(4, sizeof(cmd_t));
    xTaskCreatePinnedToCore(worker, "setup", 10240, NULL, 4, NULL, 0);
    lv_timer_create(update_cb, 1000, NULL);
    ESP_LOGI(TAG, "ready");
    return scr;
}
