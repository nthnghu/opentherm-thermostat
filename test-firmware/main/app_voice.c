// Recorder app: record up to 10 s from the mic into PSRAM, then play it back.
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "board.h"
#include "ui.h"

static const char *TAG = "app_voice";

#define MAX_SECONDS   10
#define MAX_SAMPLES   (AUDIO_SAMPLE_RATE * MAX_SECONDS)
#define CHUNK         512  // samples per read/write

typedef enum { CMD_RECORD, CMD_PLAY } cmd_t;
typedef enum { ST_IDLE, ST_RECORDING, ST_PLAYING } state_t;

static int16_t *s_buf;
static volatile size_t s_len;      // samples recorded
static volatile size_t s_pos;      // current position while recording/playing
static volatile int s_level;       // 0..100, peak of the last chunk
static volatile state_t s_state = ST_IDLE;
static volatile bool s_stop;
static QueueHandle_t s_cmds;

static lv_obj_t *s_status, *s_time, *s_level_bar, *s_progress, *s_rec_btn, *s_play_btn, *s_vol;

static void audio_task(void *arg)
{
    esp_codec_dev_handle_t dev = board_audio();
    cmd_t cmd;
    for (;;) {
        xQueueReceive(s_cmds, &cmd, portMAX_DELAY);
        s_stop = false;
        if (cmd == CMD_RECORD) {
            s_state = ST_RECORDING;
            s_pos = 0;
            while (!s_stop && s_pos < MAX_SAMPLES) {
                size_t n = MAX_SAMPLES - s_pos < CHUNK ? MAX_SAMPLES - s_pos : CHUNK;
                if (esp_codec_dev_read(dev, s_buf + s_pos, n * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
                    ESP_LOGE(TAG, "read failed");
                    break;
                }
                int peak = 0;
                for (size_t i = 0; i < n; i++) {
                    int v = abs(s_buf[s_pos + i]);
                    if (v > peak) {
                        peak = v;
                    }
                }
                s_level = peak * 100 / 32768;
                s_pos += n;
            }
            s_len = s_pos;
            s_level = 0;
            ESP_LOGI(TAG, "recorded %u samples", (unsigned)s_len);
        } else if (cmd == CMD_PLAY && s_len > 0) {
            s_state = ST_PLAYING;
            s_pos = 0;
            while (!s_stop && s_pos < s_len) {
                size_t n = s_len - s_pos < CHUNK ? s_len - s_pos : CHUNK;
                if (esp_codec_dev_write(dev, s_buf + s_pos, n * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
                    ESP_LOGE(TAG, "write failed");
                    break;
                }
                s_pos += n;
            }
            // Push a little silence so the end of the clip isn't cut off.
            static int16_t silence[CHUNK];
            esp_codec_dev_write(dev, silence, sizeof(silence));
        }
        s_state = ST_IDLE;
    }
}

static void set_btn_text(lv_obj_t *btn, const char *text)
{
    lv_label_set_text(lv_obj_get_child(btn, 0), text);
}

static void ui_timer_cb(lv_timer_t *t)
{
    state_t st = s_state;
    lv_bar_set_value(s_level_bar, st == ST_RECORDING ? s_level : 0, LV_ANIM_OFF);
    size_t total = st == ST_RECORDING ? MAX_SAMPLES : (s_len ? s_len : 1);
    lv_bar_set_value(s_progress, st == ST_IDLE ? 0 : (int)(s_pos * 1000 / total), LV_ANIM_OFF);

    size_t shown = st == ST_IDLE ? s_len : s_pos;
    lv_label_set_text_fmt(s_time, "%u.%u s", (unsigned)(shown / AUDIO_SAMPLE_RATE),
                          (unsigned)(shown % AUDIO_SAMPLE_RATE * 10 / AUDIO_SAMPLE_RATE));

    switch (st) {
    case ST_RECORDING:
        lv_label_set_text(s_status, "Recording... speak now");
        set_btn_text(s_rec_btn, LV_SYMBOL_STOP " Stop");
        lv_obj_add_state(s_play_btn, LV_STATE_DISABLED);
        break;
    case ST_PLAYING:
        lv_label_set_text(s_status, "Playing...");
        set_btn_text(s_play_btn, LV_SYMBOL_STOP " Stop");
        lv_obj_add_state(s_rec_btn, LV_STATE_DISABLED);
        break;
    default:
        lv_label_set_text(s_status, s_len ? "Ready. Tap Play to listen." : "Tap Record and say something.");
        set_btn_text(s_rec_btn, LV_SYMBOL_AUDIO " Record");
        set_btn_text(s_play_btn, LV_SYMBOL_PLAY " Play");
        lv_obj_remove_state(s_rec_btn, LV_STATE_DISABLED);
        if (s_len) {
            lv_obj_remove_state(s_play_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_play_btn, LV_STATE_DISABLED);
        }
        break;
    }
}

static void rec_cb(lv_event_t *e)
{
    if (s_state == ST_RECORDING) {
        s_stop = true;
    } else if (s_state == ST_IDLE) {
        cmd_t c = CMD_RECORD;
        xQueueSend(s_cmds, &c, 0);
    }
}

static void play_cb(lv_event_t *e)
{
    if (s_state == ST_PLAYING) {
        s_stop = true;
    } else if (s_state == ST_IDLE) {
        cmd_t c = CMD_PLAY;
        xQueueSend(s_cmds, &c, 0);
    }
}

static void vol_cb(lv_event_t *e)
{
    esp_codec_dev_set_out_vol(board_audio(), lv_slider_get_value(s_vol));
}

static void screen_cb(lv_event_t *e)
{
    s_stop = true;  // leaving the screen stops recording/playback
}

static lv_obj_t *make_bar(lv_obj_t *parent, int y, int max, lv_color_t color)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, 400, 28);
    lv_bar_set_range(bar, 0, max);
    lv_obj_set_style_bg_color(bar, color, LV_PART_INDICATOR);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, y);
    return bar;
}

lv_obj_t *app_voice_create(void)
{
    lv_obj_t *content;
    lv_obj_t *scr = ui_app_screen("Recorder", &content);
    lv_obj_add_event_cb(scr, screen_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);

    s_status = lv_label_create(content);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 40);

    s_time = lv_label_create(content);
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_40, 0);
    lv_obj_align(s_time, LV_ALIGN_TOP_MID, 0, 90);
    lv_label_set_text(s_time, "0.0 s");

    lv_obj_t *l1 = lv_label_create(content);
    lv_label_set_text(l1, "Mic level");
    lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 40, 170);
    s_level_bar = make_bar(content, 200, 100, lv_palette_main(LV_PALETTE_GREEN));

    lv_obj_t *l2 = lv_label_create(content);
    lv_label_set_text_fmt(l2, "Progress (max %d s)", MAX_SECONDS);
    lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 40, 250);
    s_progress = make_bar(content, 280, 1000, lv_palette_main(LV_PALETTE_BLUE));

    s_rec_btn = ui_button(content, LV_SYMBOL_AUDIO " Record", lv_palette_main(LV_PALETTE_RED), rec_cb, NULL);
    lv_obj_set_size(s_rec_btn, 400, 110);
    lv_obj_align(s_rec_btn, LV_ALIGN_TOP_MID, 0, 350);
    lv_obj_set_style_text_font(lv_obj_get_child(s_rec_btn, 0), &lv_font_montserrat_28, 0);

    s_play_btn = ui_button(content, LV_SYMBOL_PLAY " Play", lv_palette_main(LV_PALETTE_GREEN), play_cb, NULL);
    lv_obj_set_size(s_play_btn, 400, 110);
    lv_obj_align(s_play_btn, LV_ALIGN_TOP_MID, 0, 480);
    lv_obj_set_style_text_font(lv_obj_get_child(s_play_btn, 0), &lv_font_montserrat_28, 0);

    lv_obj_t *l3 = lv_label_create(content);
    lv_label_set_text(l3, LV_SYMBOL_VOLUME_MAX " Volume");
    lv_obj_align(l3, LV_ALIGN_TOP_LEFT, 40, 630);
    s_vol = lv_slider_create(content);
    lv_obj_set_width(s_vol, 240);
    lv_slider_set_range(s_vol, 0, 100);
    lv_slider_set_value(s_vol, 80, LV_ANIM_OFF);
    lv_obj_align(s_vol, LV_ALIGN_TOP_RIGHT, -50, 636);
    lv_obj_add_event_cb(s_vol, vol_cb, LV_EVENT_VALUE_CHANGED, NULL);

    if (!board_audio()) {
        lv_label_set_text(s_status, "Audio codec not found - see serial log");
        lv_obj_add_state(s_rec_btn, LV_STATE_DISABLED);
        lv_obj_add_state(s_play_btn, LV_STATE_DISABLED);
        return scr;
    }

    s_buf = heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_cmds = xQueueCreate(2, sizeof(cmd_t));
    xTaskCreate(audio_task, "audio", 4096, NULL, 6, NULL);
    lv_timer_create(ui_timer_cb, 50, NULL);
    return scr;
}
