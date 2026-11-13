// Setup screen: pick a Wi-Fi network + type its password on screen;
// Home Assistant URL + token are entered from a phone via a QR code.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "board.h"
#include "settings.h"
#include "portal.h"
#include "ui.h"

#define MAX_APS 30

typedef enum { CMD_SCAN, CMD_TEST } cmd_t;

static QueueHandle_t s_cmds;
static wifi_ap_record_t s_aps[MAX_APS];
static bool s_visible;
static char s_qr_ip[16] = "-";  // IP the QR code currently encodes ("-" = never drawn)
static char s_chosen_ssid[33];

static lv_obj_t *s_wifi_status, *s_qr, *s_qr_hint, *s_url_lbl, *s_ha_status;
static lv_obj_t *s_picker, *s_list, *s_scan_lbl;
static lv_obj_t *s_pw_panel, *s_pw_title, *s_pw_ta, *s_kb;

/* ------------------------------------------------------------ worker task */

static void wifi_item_cb(lv_event_t *e);

static void fill_list(int n)
{
    lv_obj_clean(s_list);
    if (n < 0) {
        lv_label_set_text(s_scan_lbl, "Scan failed - is the Wi-Fi chip OK? Tap Rescan.");
        return;
    }
    lv_label_set_text_fmt(s_scan_lbl, "%d networks found. Tap one.", n);
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
            portal_test_ha(msg, sizeof(msg));  // result shows up via portal_last_result()
        }
    }
}

static void send_cmd(cmd_t c)
{
    xQueueSend(s_cmds, &c, 0);
}

/* ------------------------------------------------------------ Wi-Fi picker */

static void open_picker(void)
{
    lv_obj_remove_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(s_list);
    lv_label_set_text(s_scan_lbl, "Scanning...");
    send_cmd(CMD_SCAN);
}

static void choose_cb(lv_event_t *e)
{
    open_picker();
}

static void rescan_cb(lv_event_t *e)
{
    lv_obj_clean(s_list);
    lv_label_set_text(s_scan_lbl, "Scanning...");
    send_cmd(CMD_SCAN);
}

static void close_picker_cb(lv_event_t *e)
{
    lv_obj_add_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
}

static void wifi_item_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    strlcpy(s_chosen_ssid, (const char *)s_aps[i].ssid, sizeof(s_chosen_ssid));
    lv_label_set_text_fmt(s_pw_title, "Password for \"%s\"", s_chosen_ssid);
    lv_textarea_set_text(s_pw_ta, "");
    lv_obj_remove_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_kb, s_pw_ta);
}

static void do_connect(void)
{
    board_wifi_connect(s_chosen_ssid, lv_textarea_get_text(s_pw_ta));
    lv_obj_add_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
}

static void connect_cb(lv_event_t *e)
{
    do_connect();
}

static void kb_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {  // keyboard's check-mark key
        do_connect();
    } else if (code == LV_EVENT_CANCEL) {
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

/* ---------------------------------------------------------- status update */

static void update_cb(lv_timer_t *t)
{
    lv_label_set_text(s_wifi_status, board_wifi_status());

    const char *ip = board_wifi_ip();
    bool up = board_wifi_connected() && ip[0];

    // The setup page only runs while this screen is open and Wi-Fi is up.
    if (s_visible && up && !portal_running()) {
        portal_start();
    } else if ((!s_visible || !up) && portal_running()) {
        portal_stop();
    }

    if (strcmp(ip, s_qr_ip) != 0) {
        strlcpy(s_qr_ip, ip, sizeof(s_qr_ip));
        if (up) {
            char url[40];
            snprintf(url, sizeof(url), "http://%s/", ip);
            lv_qrcode_update(s_qr, url, strlen(url));
            lv_obj_remove_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_qr_hint, "Scan with your phone (on the same Wi-Fi) to enter the HA URL and token.");
            lv_label_set_text(s_url_lbl, url);
        } else {
            lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_qr_hint, "Connect to Wi-Fi first. The QR code appears here.");
            lv_label_set_text(s_url_lbl, "");
        }
    }

    const settings_t *cfg = settings_get();
    const char *res = portal_last_result();
    lv_label_set_text_fmt(s_ha_status, "URL: %s\nToken: %s%s%s", cfg->ha_url[0] ? cfg->ha_url : "not set",
                          cfg->ha_token[0] ? "saved" : "missing", res[0] ? "\nLast test: " : "", res);
}

static void test_cb(lv_event_t *e)
{
    send_cmd(CMD_TEST);
}

static void screen_cb(lv_event_t *e)
{
    s_visible = lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED;
    if (s_visible && !settings_wifi_configured()) {
        open_picker();  // first run: go straight to the network list
    }
}

/* ------------------------------------------------------------------ build */

static lv_obj_t *section_title(lv_obj_t *parent, const char *text, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, 16, y);
    return l;
}

static lv_obj_t *overlay(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x121212), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

lv_obj_t *app_setup_create(void)
{
    lv_obj_t *content;
    lv_obj_t *scr = ui_app_screen("Setup", &content);
    lv_obj_add_event_cb(scr, screen_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(scr, screen_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);

    // Wi-Fi section
    section_title(content, LV_SYMBOL_WIFI " Wi-Fi", 12);
    s_wifi_status = lv_label_create(content);
    lv_obj_set_width(s_wifi_status, 448);
    lv_label_set_long_mode(s_wifi_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_wifi_status, lv_color_hex(0x90A4AE), 0);
    lv_obj_set_pos(s_wifi_status, 16, 54);
    lv_obj_t *choose = ui_button(content, "Choose network", lv_color_hex(0x1565C0), choose_cb, NULL);
    lv_obj_set_size(choose, 448, 64);
    lv_obj_set_pos(choose, 16, 88);

    // Home Assistant section
    section_title(content, LV_SYMBOL_HOME " Home Assistant", 176);
    s_qr = lv_qrcode_create(content);
    lv_qrcode_set_size(s_qr, 220);
    lv_qrcode_set_dark_color(s_qr, lv_color_black());
    lv_qrcode_set_light_color(s_qr, lv_color_white());
    lv_obj_set_style_border_color(s_qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_qr, 8, 0);
    lv_obj_set_pos(s_qr, 16, 222);
    lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);

    s_qr_hint = lv_label_create(content);
    lv_obj_set_width(s_qr_hint, 200);
    lv_label_set_long_mode(s_qr_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_qr_hint, 264, 226);
    s_url_lbl = lv_label_create(content);
    lv_obj_set_width(s_url_lbl, 200);
    lv_label_set_long_mode(s_url_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_url_lbl, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_pos(s_url_lbl, 264, 400);

    s_ha_status = lv_label_create(content);
    lv_obj_set_width(s_ha_status, 448);
    lv_label_set_long_mode(s_ha_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_ha_status, lv_color_hex(0x90A4AE), 0);
    lv_obj_set_pos(s_ha_status, 16, 474);

    lv_obj_t *test = ui_button(content, "Test HA connection", lv_color_hex(0x37474F), test_cb, NULL);
    lv_obj_set_size(test, 448, 64);
    lv_obj_set_pos(test, 16, 640);

    // Network list overlay
    s_picker = overlay(content);
    lv_obj_t *pt = lv_label_create(s_picker);
    lv_obj_set_style_text_font(pt, &lv_font_montserrat_28, 0);
    lv_label_set_text(pt, "Choose network");
    lv_obj_set_pos(pt, 16, 14);
    s_scan_lbl = lv_label_create(s_picker);
    lv_obj_set_style_text_color(s_scan_lbl, lv_color_hex(0x90A4AE), 0);
    lv_obj_set_pos(s_scan_lbl, 16, 56);
    s_list = lv_list_create(s_picker);
    lv_obj_set_size(s_list, 448, 540);
    lv_obj_set_pos(s_list, 16, 88);
    lv_obj_t *rescan = ui_button(s_picker, LV_SYMBOL_REFRESH " Rescan", lv_color_hex(0x1565C0), rescan_cb, NULL);
    lv_obj_set_size(rescan, 216, 64);
    lv_obj_set_pos(rescan, 16, 644);
    lv_obj_t *close = ui_button(s_picker, LV_SYMBOL_CLOSE " Close", lv_color_hex(0x37474F), close_picker_cb, NULL);
    lv_obj_set_size(close, 216, 64);
    lv_obj_set_pos(close, 248, 644);

    // Password overlay (on top of the list)
    s_pw_panel = overlay(s_picker);
    s_pw_title = lv_label_create(s_pw_panel);
    lv_obj_set_width(s_pw_title, 448);
    lv_label_set_long_mode(s_pw_title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_pw_title, 16, 16);
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
    lv_obj_t *conn = ui_button(s_pw_panel, LV_SYMBOL_OK " Connect", lv_color_hex(0x2E7D32), connect_cb, NULL);
    lv_obj_set_size(conn, 216, 64);
    lv_obj_set_pos(conn, 16, 172);
    lv_obj_t *cancel = ui_button(s_pw_panel, "Cancel", lv_color_hex(0x37474F), cancel_pw_cb, NULL);
    lv_obj_set_size(cancel, 216, 64);
    lv_obj_set_pos(cancel, 248, 172);
    s_kb = lv_keyboard_create(s_pw_panel);
    lv_obj_set_size(s_kb, LV_PCT(100), 320);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_kb, s_pw_ta);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_CANCEL, NULL);

    s_cmds = xQueueCreate(4, sizeof(cmd_t));
    xTaskCreate(worker, "setup", 8192, NULL, 4, NULL);
    lv_timer_create(update_cb, 500, NULL);
    return scr;
}
