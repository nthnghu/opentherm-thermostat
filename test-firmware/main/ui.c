#include "ui.h"
#include "board.h"

static lv_obj_t *s_home;
static lv_obj_t *s_apps[4];
static lv_obj_t *s_wifi_label;

static void back_cb(lv_event_t *e)
{
    ui_go_home();
}

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

lv_obj_t *ui_app_screen(const char *title, lv_obj_t **content)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, LV_PCT(100), UI_HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x1E2A38), 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = ui_button(header, LV_SYMBOL_LEFT " Home", lv_color_hex(0x37474F), back_cb, NULL);
    lv_obj_set_size(back, 130, 52);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, -6, 0);

    lv_obj_t *t = lv_label_create(header);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_CENTER, 50, 0);

    lv_obj_t *c = lv_obj_create(scr);
    lv_obj_set_size(c, BOARD_LCD_H_RES, BOARD_LCD_V_RES - UI_HEADER_H);
    lv_obj_align(c, LV_ALIGN_TOP_LEFT, 0, UI_HEADER_H);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    *content = c;
    return scr;
}

static void open_app_cb(lv_event_t *e)
{
    lv_obj_t *scr = lv_event_get_user_data(e);
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

void ui_open_setup(void)
{
    lv_screen_load(s_apps[3]);
}

void ui_go_home(void)
{
    lv_screen_load_anim(s_home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

static void wifi_timer_cb(lv_timer_t *t)
{
    lv_label_set_text(s_wifi_label, board_wifi_status());
}

void ui_home_create(void)
{
    s_apps[0] = app_ha_create();
    s_apps[1] = app_voice_create();
    s_apps[2] = app_blocks_create();
    s_apps[3] = app_setup_create();

    s_home = lv_obj_create(NULL);
    lv_obj_remove_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_home, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_home, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_home, 28, 0);
    lv_obj_set_style_pad_row(s_home, 20, 0);

    lv_obj_t *title = lv_label_create(s_home);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_40, 0);
    lv_label_set_text(title, "P4 Test");

    s_wifi_label = lv_label_create(s_home);
    lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(0x90A4AE), 0);
    lv_label_set_text(s_wifi_label, board_wifi_status());
    lv_timer_create(wifi_timer_cb, 1000, NULL);

    static const struct {
        const char *text;
        uint32_t color;
    } items[] = {
        {LV_SYMBOL_HOME "  Sensors", 0x1565C0},
        {LV_SYMBOL_AUDIO "  Recorder", 0x2E7D32},
        {LV_SYMBOL_PLAY "  Blocks", 0x6A1B9A},
        {LV_SYMBOL_SETTINGS "  Setup", 0x455A64},
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = ui_button(s_home, items[i].text, lv_color_hex(items[i].color), open_app_cb, s_apps[i]);
        lv_obj_set_size(b, LV_PCT(100), 128);
        lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &lv_font_montserrat_40, 0);
    }

    lv_screen_load(s_home);
}
