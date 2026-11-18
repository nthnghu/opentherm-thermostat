// Pull-down quick panel (tap or swipe down on the status bar): brightness, screen off, theme,
// wake on motion, connection status and shortcuts into Setup.
#include "quickpanel.h"
#include <stdio.h>
#include "board.h"
#include "settings.h"
#include "ui.h"
#include "ui_theme.h"
#include "ha.h"
#include "motion.h"
#include "timekeep.h"

#define PANEL_H 640

static lv_obj_t *s_backdrop, *s_panel, *s_bright, *s_theme, *s_motion, *s_motion_lbl, *s_status;
static lv_timer_t *s_timer;
static bool s_open;
static settings_t s_tmp;  // LVGL task only

static void refresh(void)
{
    const settings_t *c = settings_get();
    lv_slider_set_value(s_bright, c->bright, LV_ANIM_OFF);
    lv_buttonmatrix_set_button_ctrl_all(s_theme, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_selected_button(s_theme, c->theme_mode);
    for (int i = 0; i < 4; i++) {
        if (i == c->theme_mode) {
            lv_buttonmatrix_set_button_ctrl(s_theme, i, LV_BUTTONMATRIX_CTRL_CHECKED);
        } else {
            lv_buttonmatrix_clear_button_ctrl(s_theme, i, LV_BUTTONMATRIX_CTRL_CHECKED);
        }
    }
    lv_obj_set_state(s_motion, LV_STATE_CHECKED, c->motion_wake);
    ui_label_fmt(s_motion_lbl, "#%06lX Camera: %s#", (unsigned long)ui_hex(UI_MUTED), motion_state_name());

    const char *ha = ha_last_result();
    ha_state_t hs = ha_state();
    char sig[20] = "";
    if (board_wifi_connected()) {
        snprintf(sig, sizeof(sig), " (%d dBm)", board_wifi_rssi());
    }
    ui_label_fmt(s_status, "%s  Wi-Fi: %s%s\n%s  Home Assistant: %s\n%s  Clock: %s",
                 LV_SYMBOL_WIFI, board_wifi_status(), sig, LV_SYMBOL_HOME,
                 hs == HA_STATE_OK ? "connected" : hs == HA_STATE_ERROR ? (ha[0] ? ha : "error") : "not checked yet",
                 UI_ICON_CLOCK, timekeep_source_name());
}

static void timer_cb(lv_timer_t *t)
{
    if (s_open) {
        refresh();
    }
}

static void anim_y(void *obj, int32_t v)
{
    lv_obj_set_y(obj, v);
}

static void hide_done(lv_anim_t *a)
{
    lv_obj_set_hidden(s_panel, true);
    lv_obj_set_hidden(s_backdrop, true);
}

static void slide(bool down)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_panel);
    lv_anim_set_exec_cb(&a, anim_y);
    lv_anim_set_values(&a, lv_obj_get_y(s_panel), down ? UI_STATUS_H : UI_STATUS_H - PANEL_H);
    lv_anim_set_duration(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (!down) {
        lv_anim_set_completed_cb(&a, hide_done);
    }
    lv_anim_start(&a);
}

void quickpanel_open(void)
{
    if (s_open) {
        return;
    }
    s_open = true;
    ui_set_busy(true);
    refresh();
    lv_obj_set_hidden(s_backdrop, false);
    lv_obj_set_hidden(s_panel, false);
    slide(true);
}

void quickpanel_close(void)
{
    if (!s_open) {
        return;
    }
    s_open = false;
    ui_set_busy(false);
    slide(false);
}

bool quickpanel_is_open(void)
{
    return s_open;
}

/* ---------------------------------------------------------------- events */

static void backdrop_cb(lv_event_t *e)
{
    quickpanel_close();
}

static void gesture_cb(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP) {
        quickpanel_close();
    }
}

static void bright_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(s_bright);
    board_backlight(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        settings_copy(&s_tmp);
        s_tmp.bright = v;
        settings_save(&s_tmp);
    }
}

static void off_cb(lv_event_t *e)
{
    quickpanel_close();
    ui_screen_off_now();
}

static void theme_cb(lv_event_t *e)
{
    uint32_t sel = lv_buttonmatrix_get_selected_button(s_theme);
    if (sel > 3) {
        return;
    }
    settings_copy(&s_tmp);
    s_tmp.theme_mode = sel;
    settings_save(&s_tmp);
    ui_theme_update();  // apply right away
    refresh();
}

static void motion_cb(lv_event_t *e)
{
    settings_copy(&s_tmp);
    s_tmp.motion_wake = lv_obj_has_state(s_motion, LV_STATE_CHECKED);
    settings_save(&s_tmp);
}

static void shortcut_cb(lv_event_t *e)
{
    int panel = (int)(intptr_t)lv_event_get_user_data(e);
    quickpanel_close();
    ui_open_setup_section(panel);
}

static void close_cb(lv_event_t *e)
{
    quickpanel_close();
}

/* ----------------------------------------------------------------- build */

static lv_obj_t *label(lv_obj_t *p, const char *text, int x, int y)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, ui_c(UI_TEXT), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void shortcut(lv_obj_t *p, const char *text, int panel, int x, int y, int w)
{
    lv_obj_t *b = ui_button(p, text, lv_color_hex(0x37474F), shortcut_cb, (void *)(intptr_t)panel);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, 56);
}

void quickpanel_create(void)
{
    s_backdrop = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_backdrop);
    lv_obj_set_size(s_backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_backdrop, LV_OPA_50, 0);
    lv_obj_set_clickable(s_backdrop, true);
    lv_obj_set_hidden(s_backdrop, true);
    lv_obj_add_event_cb(s_backdrop, backdrop_cb, LV_EVENT_CLICKED, NULL);

    s_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_panel, BOARD_LCD_H_RES, PANEL_H);
    lv_obj_set_pos(s_panel, 0, UI_STATUS_H - PANEL_H);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_bg_color(s_panel, ui_c(UI_SURFACE), 0);
    lv_obj_set_style_pad_all(s_panel, 0, 0);
    lv_obj_set_style_text_font(s_panel, &font_icons_20, 0);  // the top layer has no theme font: UI_ICON_* need this
    lv_obj_set_scrollable(s_panel, false);
    lv_obj_set_hidden(s_panel, true);
    lv_obj_set_gesture_bubble(s_panel, false);  // swipe up on the panel or its buttons: close
    lv_obj_add_event_cb(s_panel, gesture_cb, LV_EVENT_GESTURE, NULL);

    // Brightness + screen off
    label(s_panel, UI_ICON_SUN "  Brightness", 20, 22);
    s_bright = lv_slider_create(s_panel);
    lv_slider_set_range(s_bright, 5, 100);
    lv_obj_set_size(s_bright, 230, 14);
    lv_obj_set_pos(s_bright, 220, 28);
    lv_obj_add_event_cb(s_bright, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_bright, bright_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_t *off = ui_button(s_panel, LV_SYMBOL_POWER "  Screen off now", lv_color_hex(0x37474F), off_cb, NULL);
    lv_obj_set_pos(off, 20, 66);
    lv_obj_set_size(off, 440, 56);

    // Theme
    label(s_panel, UI_ICON_PALETTE "  Theme", 20, 150);
    static const char *map[] = {"Sun", "Light", "Dark", "Hours", ""};
    s_theme = lv_buttonmatrix_create(s_panel);
    lv_buttonmatrix_set_map(s_theme, map);
    lv_buttonmatrix_set_one_checked(s_theme, true);
    lv_obj_set_size(s_theme, 330, 60);
    lv_obj_set_pos(s_theme, 130, 132);
    lv_obj_set_style_pad_all(s_theme, 4, 0);
    lv_obj_add_event_cb(s_theme, theme_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Wake on motion
    label(s_panel, UI_ICON_WALK "  Wake on motion", 20, 222);
    s_motion = lv_switch_create(s_panel);
    lv_obj_set_pos(s_motion, 390, 216);
    lv_obj_add_event_cb(s_motion, motion_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_motion_lbl = lv_label_create(s_panel);
    lv_label_set_recolor(s_motion_lbl, true);
    lv_obj_set_pos(s_motion_lbl, 48, 254);

    // Status
    s_status = lv_label_create(s_panel);
    lv_label_set_recolor(s_status, true);
    lv_obj_set_width(s_status, 440);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_status, 90);
    lv_obj_set_style_text_color(s_status, ui_c(UI_CARD_TEXT), 0);
    lv_obj_set_pos(s_status, 20, 296);

    // Shortcuts into Setup (section index as in app_setup.c)
    shortcut(s_panel, LV_SYMBOL_WIFI "  Wi-Fi", 0, 20, 400, 214);
    shortcut(s_panel, LV_SYMBOL_HOME "  Home Assistant", 1, 246, 400, 214);
    shortcut(s_panel, UI_ICON_CLOCK "  Clock", 2, 20, 466, 214);
    shortcut(s_panel, UI_ICON_DISPLAY "  Display", 5, 246, 466, 214);
    shortcut(s_panel, LV_SYMBOL_SETTINGS "  All settings", -1, 20, 532, 440);

    lv_obj_t *handle = ui_button(s_panel, LV_SYMBOL_UP, lv_color_hex(0x37474F), close_cb, NULL);
    lv_obj_set_size(handle, 120, 30);
    lv_obj_align(handle, LV_ALIGN_BOTTOM_MID, 0, -6);

    s_timer = lv_timer_create(timer_cb, 1000, NULL);
}
