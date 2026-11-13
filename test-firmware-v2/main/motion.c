// Wake-on-motion with the camera: ~5 frames/s are reduced to a 32 x 18 grid of brightness cells and
// compared with a slowly adapting background. Each frame is normalised by its mean brightness, so
// the camera's auto-exposure doesn't look like motion. Too dark (unlit room) = no detection.
// Frames are processed in RAM only and never stored or sent anywhere.
#include "motion.h"
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "camera.h"
#include "settings.h"

static const char *TAG = "motion";

#define GX 32
#define GY 18
#define N  (GX * GY)
#define DARK_LUMA 14

static volatile bool s_paused;
static volatile motion_state_t s_state = MOTION_OFF;
static volatile int64_t s_last_us;
static volatile uint32_t s_count;

typedef struct {
    float cell[N];
    float mean;
} grid_t;

static void grid_cb(const uint16_t *px, int w, int h, void *user)
{
    grid_t *g = user;
    int cw = w / GX, ch = h / GY;
    float total = 0;
    for (int gy = 0; gy < GY; gy++) {
        for (int gx = 0; gx < GX; gx++) {
            unsigned sum = 0, n = 0;
            for (int y = gy * ch + 2; y < (gy + 1) * ch; y += 8) {
                const uint16_t *row = px + (size_t)y * w;
                for (int x = gx * cw + 2; x < (gx + 1) * cw; x += 8) {
                    uint16_t p = row[x];
                    sum += (((p >> 11) << 3) * 77 + (((p >> 5) & 0x3f) << 2) * 150 + ((p & 0x1f) << 3) * 29) >> 8;
                    n++;
                }
            }
            float v = n ? (float)sum / n : 0;
            g->cell[gy * GX + gx] = v;
            total += v;
        }
    }
    g->mean = total / N;
}

static void wake_screen(void)
{
    s_last_us = esp_timer_get_time();
    s_count++;
    if (lvgl_port_lock(100)) {
        lv_display_trigger_activity(NULL);  // counts as a touch for dimming / screen-off
        lvgl_port_unlock();
    }
}

static void motion_task(void *arg)
{
    static grid_t cur, bg;
    static const float thr[3] = {28, 18, 12};          // per-cell change (luma levels)
    static const float frac[3] = {0.08f, 0.04f, 0.02f}; // share of cells that must change
    bool owner = false;   // we started the stream
    int warmup = 0, hits = 0;
    int64_t cooldown_until = 0;

    for (;;) {
        const settings_t *c = settings_get();
        if (!c->motion_wake || s_paused) {
            if (owner && !s_paused) {
                camera_stop();
            }
            owner = false;
            s_state = s_paused ? MOTION_PAUSED : MOTION_OFF;
            warmup = 0;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!camera_streaming()) {
            if (camera_init() != ESP_OK || camera_start() != ESP_OK) {
                s_state = MOTION_NO_CAMERA;
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            owner = true;
            warmup = 0;
        }
        if (!camera_grab(grid_cb, &cur)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (cur.mean < DARK_LUMA) {
            s_state = MOTION_TOO_DARK;
            warmup = 0;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        s_state = MOTION_WATCHING;
        if (warmup < 10) {  // let auto-exposure settle, then learn the background
            bg = cur;
            warmup++;
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }
        int sens = c->motion_sens > 2 ? 2 : c->motion_sens;
        int changed = 0;
        for (int i = 0; i < N; i++) {
            float d = fabsf((cur.cell[i] - cur.mean) - (bg.cell[i] - bg.mean));
            changed += d > thr[sens];
        }
        bool lights = fabsf(cur.mean - bg.mean) > 40;  // a light switched on/off: someone is there
        int64_t now = esp_timer_get_time();
        hits = (changed > frac[sens] * N || lights) ? hits + 1 : 0;
        if (hits >= 2 && now > cooldown_until) {
            ESP_LOGI(TAG, "motion (%d/%d cells%s)", changed, N, lights ? ", light change" : "");
            wake_screen();
            cooldown_until = now + 2000000;
            hits = 0;
        }
        // Background adapts: a moved chair stops counting as motion after a few seconds.
        for (int i = 0; i < N; i++) {
            bg.cell[i] = bg.cell[i] * 0.85f + cur.cell[i] * 0.15f;
        }
        bg.mean = bg.mean * 0.85f + cur.mean * 0.15f;
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

void motion_start(void)
{
    xTaskCreatePinnedToCore(motion_task, "motion", 4096, NULL, 2, NULL, 0);
}

void motion_set_paused(bool paused)
{
    s_paused = paused;
}

motion_state_t motion_state(void)
{
    return s_state;
}

const char *motion_state_name(void)
{
    switch (s_state) {
    case MOTION_WATCHING:
        return "watching";
    case MOTION_TOO_DARK:
        return "too dark to see";
    case MOTION_PAUSED:
        return "paused (camera in use)";
    case MOTION_NO_CAMERA:
        return "camera not available";
    default:
        return "off";
    }
}

int motion_seconds_since(void)
{
    return s_last_us ? (int)((esp_timer_get_time() - s_last_us) / 1000000) : -1;
}

uint32_t motion_count(void)
{
    return s_count;
}
