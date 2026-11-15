// Thermostat control task.
// Every second: read the SHT40 (every 2nd loop), decide heat demand with hysteresis, compute a flow
// setpoint (P+I on the room error), and talk OpenTherm to the boiler (real or simulated).
// Failsafe: no sensor reading for 30 s -> heat demand off. Mode Off still protects against frost (7 °C).
#include "thermostat.h"
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "board.h"
#include "sht4x.h"
#include "ot_bus.h"
#include "ot_frame.h"
#include "timekeep.h"

static const char *TAG = "thermo";

#define LOOP_MS          1000
#define SENSOR_STALE_US  ((int64_t)THERMO_SENSOR_STALE_S * 1000000)
#define SAVE_DELAY_US    (5LL * 1000000)
#define FLOW_MIN         30.0f
#define FLOW_MAX         60.0f
#define FLOW_OFF         10.0f
#define SMOOTH_ALPHA     (2.0f / 30.0f)  // reading every 2 s -> ~30 s exponential average
#define SHOW_BAND_C      0.08f           // shown temperature moves only past this from the smoothed value
#define SHOW_BAND_RH     0.8f

static SemaphoreHandle_t s_lock;
static thermo_state_t s_st;
static int64_t s_last_reading_us;
static int64_t s_dirty_since_us;  // 0 = nothing to save
static float s_integral;
static thermo_sample_t *s_hist;   // PSRAM ring
static int s_hist_head, s_hist_count;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

/* ------------------------------------------------------------ persistence */

static void load(void)
{
    s_st.setpoint = 21.0f;
    s_st.mode = THERMO_MODE_HEAT;
    s_st.offset = 0.0f;
    s_st.hysteresis = 0.2f;
    s_st.boiler_sim = true;
    s_st.sim_room = false;
    nvs_handle_t h;
    if (nvs_open("thermo", NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        uint8_t b;
        if (nvs_get_i32(h, "sp", &v) == ESP_OK) {
            s_st.setpoint = v / 10.0f;
        }
        if (nvs_get_u8(h, "mode", &b) == ESP_OK) {
            s_st.mode = b ? THERMO_MODE_HEAT : THERMO_MODE_OFF;
        }
        if (nvs_get_i32(h, "ofs", &v) == ESP_OK) {
            s_st.offset = v / 10.0f;
        }
        if (nvs_get_i32(h, "hyst", &v) == ESP_OK) {
            s_st.hysteresis = v / 100.0f;
        }
        if (nvs_get_u8(h, "sim", &b) == ESP_OK) {
            s_st.boiler_sim = b;
        }
        if (nvs_get_u8(h, "simroom", &b) == ESP_OK) {
            s_st.sim_room = b;
        }
        nvs_close(h);
    }
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open("thermo", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    LOCK();
    thermo_state_t c = s_st;
    UNLOCK();
    nvs_set_i32(h, "sp", (int32_t)lroundf(c.setpoint * 10));
    nvs_set_u8(h, "mode", c.mode);
    nvs_set_i32(h, "ofs", (int32_t)lroundf(c.offset * 10));
    nvs_set_i32(h, "hyst", (int32_t)lroundf(c.hysteresis * 100));
    nvs_set_u8(h, "sim", c.boiler_sim);
    nvs_set_u8(h, "simroom", c.sim_room);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "settings saved (sp %.1f, mode %d)", c.setpoint, c.mode);
}

static void mark_dirty(void)
{
    s_dirty_since_us = esp_timer_get_time();
}

/* ---------------------------------------------------------------- boiler */

// The bus task (ot_bus.c) talks to the boiler every second; here we only hand over the request and copy
// back what it learned.
static void boiler_step(bool demand, float flow_sp)
{
    otb_cmd_t cmd = {.ch_enable = demand, .dhw_enable = true, .tset = flow_sp};
    otbus_set_cmd(&cmd);
    otb_status_t b;
    otbus_get(&b);
    LOCK();
    s_st.boiler_online = b.link == OTB_LINK_UP;
    s_st.flame = b.slave_flags & OT_S_FLAME;
    s_st.flow_c = b.flow_c;
    s_st.modulation = b.modulation;
    s_st.ot_ok = b.ok;
    s_st.ot_errors = b.timeouts + b.bad;
    UNLOCK();
}

/* --------------------------------------------------------------- history */

static void history_add(void)
{
    LOCK();
    thermo_sample_t s = {
        .t = timekeep_valid() ? time(NULL) : 0,
        .room_c10 = s_st.sensor_ok ? (int16_t)lroundf(s_st.room_c * 10) : INT16_MIN,
        .humidity = (uint8_t)lroundf(s_st.humidity),
        .setpoint_c10 = (int16_t)lroundf((s_st.mode == THERMO_MODE_HEAT ? s_st.setpoint : THERMO_FROST) * 10),
        .demand = s_st.demand,
    };
    s_hist[s_hist_head] = s;
    s_hist_head = (s_hist_head + 1) % THERMO_HISTORY_LEN;
    if (s_hist_count < THERMO_HISTORY_LEN) {
        s_hist_count++;
    }
    UNLOCK();
}

int thermo_history(thermo_sample_t *out, int max)
{
    LOCK();
    int n = s_hist_count < max ? s_hist_count : max;
    int start = (s_hist_head - n + THERMO_HISTORY_LEN) % THERMO_HISTORY_LEN;
    for (int i = 0; i < n; i++) {
        out[i] = s_hist[(start + i) % THERMO_HISTORY_LEN];
    }
    UNLOCK();
    return n;
}

/* ------------------------------------------------------------------ task */

static void thermo_task(void *arg)
{
    esp_task_wdt_add(NULL);
    bool sensor_inited = sht4x_init(board_i2c_bus()) == ESP_OK;
    TickType_t wake = xTaskGetTickCount();
    int64_t last_hist = 0;
    for (int loop = 0;; loop++) {
        esp_task_wdt_reset();
        int64_t now = esp_timer_get_time();

        // 1. Sensor (every 2 s)
        if (loop % 2 == 0) {
            float t, h;
            esp_err_t err = sensor_inited ? sht4x_measure(&t, &h) : ESP_ERR_INVALID_STATE;
            if (!sensor_inited) {
                sensor_inited = sht4x_init(board_i2c_bus()) == ESP_OK;
            }
            // Bench testing: the simulated house replaces the SHT40 temperature (humidity stays real).
            float sim_t;
            if (s_st.sim_room && s_st.boiler_sim && otbus_sim_room(&sim_t)) {
                t = sim_t;
                if (err != ESP_OK) {
                    h = 45;
                    err = ESP_OK;
                }
            }
            LOCK();
            if (err == ESP_OK && t > -20 && t < 60) {
                float raw = t + s_st.offset;
                s_st.room_raw = raw;
                // Smooth the sensor noise; restart the average after a gap (sensor was missing).
                bool restart = !s_last_reading_us || now - s_last_reading_us > SENSOR_STALE_US;
                if (restart) {
                    s_st.room_c = raw;
                    s_st.humidity = h;
                } else {
                    s_st.room_c += SMOOTH_ALPHA * (raw - s_st.room_c);
                    s_st.humidity += SMOOTH_ALPHA * (h - s_st.humidity);
                }
                // Dead band for the display: a value sitting on a 0.05 border no longer flips back and forth.
                if (restart || fabsf(s_st.room_c - s_st.room_shown) >= SHOW_BAND_C) {
                    s_st.room_shown = roundf(s_st.room_c * 10) / 10;
                }
                if (restart || fabsf(s_st.humidity - s_st.humidity_shown) >= SHOW_BAND_RH) {
                    s_st.humidity_shown = roundf(s_st.humidity);
                }
                s_last_reading_us = now;
            } else {
                s_st.sensor_errors++;
                if (s_st.sensor_errors < 5 || s_st.sensor_errors % 15 == 0) {
                    ESP_LOGW(TAG, "SHT40 read failed (%s), %lu errors, last good %lld s ago", esp_err_to_name(err),
                             (unsigned long)s_st.sensor_errors,
                             s_last_reading_us ? (now - s_last_reading_us) / 1000000 : -1LL);
                }
            }
            s_st.sensor_ok = s_last_reading_us && now - s_last_reading_us < SENSOR_STALE_US;
            s_st.sensor_age_s = s_last_reading_us ? (int)((now - s_last_reading_us) / 1000000) : -1;
            UNLOCK();
        }

        // 2. Control
        LOCK();
        float target = s_st.mode == THERMO_MODE_HEAT ? s_st.setpoint : THERMO_FROST;
        bool demand = s_st.demand;
        if (!s_st.sensor_ok) {
            demand = false;  // failsafe
        } else if (s_st.room_c < target - s_st.hysteresis) {
            demand = true;
        } else if (s_st.room_c > target + s_st.hysteresis) {
            demand = false;
        }
        float err = s_st.sensor_ok ? target - s_st.room_c : 0;
        if (demand) {
            s_integral += err * 0.02f;  // °C flow per °C·s
            s_integral = fminf(fmaxf(s_integral, -10.0f), 15.0f);
        } else {
            s_integral *= 0.98f;
        }
        float flow = demand ? fminf(fmaxf(35.0f + 15.0f * err + s_integral, FLOW_MIN), FLOW_MAX) : FLOW_OFF;
        s_st.demand = demand;
        s_st.flow_setpoint = flow;
        bool sim = s_st.boiler_sim;
        s_st.loops++;
        UNLOCK();

        // 3. Boiler
        otbus_set_simulated(sim);
        boiler_step(demand, flow);

        // 4. History (1 / min) and debounced save
        if (now - last_hist >= 60LL * 1000000 || last_hist == 0) {
            last_hist = now;
            history_add();
        }
        if (s_dirty_since_us && now - s_dirty_since_us > SAVE_DELAY_US) {
            s_dirty_since_us = 0;
            save();
        }

        // Fixed 1 s cadence; if a boiler timeout made us late, don't try to catch up.
        if (xTaskDelayUntil(&wake, pdMS_TO_TICKS(LOOP_MS)) == pdFALSE) {
            wake = xTaskGetTickCount();
        }
    }
}

void thermo_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_hist = heap_caps_calloc(THERMO_HISTORY_LEN, sizeof(thermo_sample_t), MALLOC_CAP_SPIRAM);
    load();
    ESP_LOGI(TAG, "start: setpoint %.1f, mode %s, offset %.1f, boiler %s", s_st.setpoint,
             s_st.mode ? "heat" : "off", s_st.offset, s_st.boiler_sim ? "simulated" : "real");
    // Highest application priority, pinned to core 1 (the UI runs on core 0).
    xTaskCreatePinnedToCore(thermo_task, "thermo", 6144, NULL, 20, NULL, 1);
}

void thermo_get(thermo_state_t *out)
{
    LOCK();
    *out = s_st;
    UNLOCK();
}

void thermo_set_setpoint(float c)
{
    c = roundf(c * 2) / 2;  // 0.5 °C steps
    c = fminf(fmaxf(c, THERMO_SP_MIN), THERMO_SP_MAX);
    LOCK();
    s_st.setpoint = c;
    UNLOCK();
    mark_dirty();
}

void thermo_adjust_setpoint(float delta)
{
    LOCK();
    float c = s_st.setpoint + delta;
    UNLOCK();
    thermo_set_setpoint(c);
}

void thermo_set_mode(thermo_mode_t m)
{
    LOCK();
    s_st.mode = m;
    UNLOCK();
    mark_dirty();
}

void thermo_set_offset(float c)
{
    c = fminf(fmaxf(roundf(c * 10) / 10, -5.0f), 5.0f);
    LOCK();
    float d = c - s_st.offset;
    s_st.room_c += d;
    s_st.room_raw += d;
    s_st.room_shown = roundf((s_st.room_shown + d) * 10) / 10;
    s_st.offset = c;
    UNLOCK();
    mark_dirty();
}

void thermo_set_hysteresis(float c)
{
    c = fminf(fmaxf(roundf(c * 20) / 20, 0.05f), 1.0f);
    LOCK();
    s_st.hysteresis = c;
    UNLOCK();
    mark_dirty();
}

void thermo_set_sim_room(bool on)
{
    LOCK();
    s_st.sim_room = on;
    UNLOCK();
    mark_dirty();
}

void thermo_set_boiler_sim(bool sim)
{
    LOCK();
    s_st.boiler_sim = sim;
    s_st.boiler_online = false;
    UNLOCK();
    mark_dirty();
}
