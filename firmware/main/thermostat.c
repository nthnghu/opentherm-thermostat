// Thermostat control task.
// Every second: read the SHT40 (every 2nd loop), run the control law (control.c) on the room and the boiler
// state, and hand the request to the OpenTherm bus task (ot_bus.c).
// Sensor lost (no reading for 30 s), frost floor and every other feature: see control.h and ctrl_config_t.
#include "thermostat.h"
#include "sensor_filter.h"
#include "awaykeys.h"
#include "selftest.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "board.h"
#include "sht4x.h"
#include "ot_bus.h"
#include "ot_frame.h"
#include "control.h"
#include "timekeep.h"
#include "histstore.h"

static const char *TAG = "thermo";

#define LOOP_MS          1000
#define SAVE_DELAY_MS    5000

static SemaphoreHandle_t s_lock;
static thermo_state_t s_st;
static sensor_filter_t s_sf;  // room sensor filter, thermostat task (offset change under LOCK)
// When something to save last changed, in ms since start (|1 so it is never 0 = nothing to save). 32 bits: written
// by other tasks without the lock, which is only safe for a value this chip stores in one step.
static volatile uint32_t s_dirty_ms;

static uint32_t dirty_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000) | 1;
}
static ctrl_t s_ctrl;            // used by the thermostat task only
static ctrl_config_t s_cfg;       // current configuration (under s_lock)
static bool s_cfg_changed;
static bool s_cfg_dirty;
static sched_t s_sched;           // under s_lock (the user actions change it from other tasks)
static sched_config_t s_scfg;
static bool s_scfg_dirty;
static alerts_t s_alerts;         // thermostat task (config changes under s_lock)
static alert_config_t s_acfg;
static bool s_acfg_changed, s_acfg_dirty;
static dl_t s_dl;                 // under s_lock
static svc_t s_svc;               // under s_lock
static bool s_svccfg_dirty;
static dl_kpi_t s_kpi1, s_kpi24;
static volatile int s_rssi;
#define LOG_RECS 1440   // 24 h of minutes (longer raw history: MQTT -> VictoriaMetrics / Grafana)
#define LOG_BURNS 512
#define LOG_HOURS 840   // 35 days of hourly summaries (week chart)
static thermo_fine_t *s_fine;     // PSRAM ring, 5 s
static int s_fine_head, s_fine_count;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

/* ------------------------------------------------------------ persistence */

static void load_ctrl_config(void)
{
    ctrl_default_config(&s_cfg);
    nvs_handle_t h;
    if (nvs_open("thermo", NVS_READONLY, &h) == ESP_OK) {
        ctrl_config_t c;
        size_t len = sizeof(c);
        if (nvs_get_blob(h, "ctrl", &c, &len) == ESP_OK && len == sizeof(c) && c.version == CTRL_CONFIG_VERSION) {
            s_cfg = c;
        } else {
            int32_t v;
            if (nvs_get_i32(h, "hyst", &v) == ESP_OK) {  // 0.1 setting
                s_cfg.onoff_hyst_k = v / 100.0f;
            }
            ESP_LOGI(TAG, "control settings: defaults (none saved or older version)");
        }
        nvs_close(h);
    }
    ctrl_sanitize(&s_cfg);
}

static void save_ctrl_config(void)
{
    nvs_handle_t h;
    if (nvs_open("thermo", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    LOCK();
    ctrl_config_t c = s_cfg;
    UNLOCK();
    nvs_set_blob(h, "ctrl", &c, sizeof(c));
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "control settings saved");
}

static void load_sched_config(void)
{
    sched_default_config(&s_scfg);
    nvs_handle_t h;
    if (nvs_open("thermo", NVS_READONLY, &h) == ESP_OK) {
        sched_config_t c;
        size_t len = sizeof(c);
        if (nvs_get_blob(h, "sched", &c, &len) == ESP_OK && len == sizeof(c) && c.version == SCHED_CONFIG_VERSION) {
            s_scfg = c;
        } else {
            ESP_LOGI(TAG, "schedule settings: defaults");
        }
        nvs_close(h);
    }
    sched_sanitize(&s_scfg);
}

static void save_sched_config(void)
{
    nvs_handle_t h;
    if (nvs_open("thermo", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    LOCK();
    sched_config_t c = s_scfg;
    UNLOCK();
    nvs_set_blob(h, "sched", &c, sizeof(c));
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "schedule settings saved");
}

static void load_svc(void)
{
    svc_config_t c;
    svc_default_config(&c);
    svc_record_t *rec = calloc(1, sizeof(svc_record_t));
    bool have_rec = false;
    nvs_handle_t h;
    if (nvs_open("thermo", NVS_READONLY, &h) == ESP_OK) {
        svc_config_t lc;
        size_t len = sizeof(lc);
        if (nvs_get_blob(h, "svc", &lc, &len) == ESP_OK && len == sizeof(lc) && lc.version == SVC_CONFIG_VERSION) {
            c = lc;
        }
        len = sizeof(svc_record_t);
        have_rec = rec && nvs_get_blob(h, "svcrec", rec, &len) == ESP_OK && len == sizeof(svc_record_t);
        nvs_close(h);
    }
    svc_init(&s_svc, &c, have_rec ? rec : NULL);
    free(rec);
}

static void save_svc(bool cfg, bool rec)
{
    nvs_handle_t h;
    if (nvs_open("thermo", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    svc_record_t *r = rec ? malloc(sizeof(svc_record_t)) : NULL;
    LOCK();
    svc_config_t c = s_svc.cfg;
    if (r) {
        *r = s_svc.rec;
    }
    UNLOCK();
    if (cfg) {
        nvs_set_blob(h, "svc", &c, sizeof(c));
    }
    if (r) {
        nvs_set_blob(h, "svcrec", r, sizeof(*r));
        free(r);
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "service %s saved", rec ? "record" : "settings");
}

static void load_alert_config(void)
{
    alert_default_config(&s_acfg);
    nvs_handle_t h;
    if (nvs_open("thermo", NVS_READONLY, &h) == ESP_OK) {
        alert_config_t c;
        size_t len = sizeof(c);
        if (nvs_get_blob(h, "alert", &c, &len) == ESP_OK && len == sizeof(c) && c.version == ALERT_CONFIG_VERSION) {
            s_acfg = c;
        }
        nvs_close(h);
    }
    alert_sanitize(&s_acfg);
}

static void save_alert_config(void)
{
    nvs_handle_t h;
    if (nvs_open("thermo", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    LOCK();
    alert_config_t c = s_acfg;
    UNLOCK();
    nvs_set_blob(h, "alert", &c, sizeof(c));
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "alert settings saved");
}

// True while the simulated/real boiler choice has never been saved. The thermostat then talks to the real bus
// for BOILER_DETECT_S: a boiler that answers is kept (and saved); with no answer the simulator takes over, unsaved,
// so the next start looks again. Lost settings on an installed thermostat therefore never end in "simulated".
#define BOILER_DETECT_S 120
static bool s_boiler_auto;
static bool s_boiler_chosen;  // the choice is a real one (saved, set by hand, or a boiler answered): save() may store it

static void load(void)
{
    s_st.setpoint = 21.0f;
    s_st.mode = THERMO_MODE_HEAT;
    s_st.offset = 0.0f;
    s_st.hysteresis = 0.2f;
    // No saved choice (new device, or the settings were lost): look for a real boiler first, see thermo_task.
    s_st.boiler_sim = false;
    s_boiler_auto = true;
    s_st.sim_room = false;
    nvs_handle_t h;
    if (nvs_open("thermo", NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        uint8_t b;
        if (nvs_get_i32(h, "sp", &v) == ESP_OK && v >= THERMO_SP_MIN * 10 && v <= THERMO_SP_MAX * 10) {
            s_st.setpoint = v / 10.0f;
        }
        if (nvs_get_u8(h, "mode", &b) == ESP_OK) {
            s_st.mode = b ? THERMO_MODE_HEAT : THERMO_MODE_OFF;
        }
        if (nvs_get_i32(h, "ofs", &v) == ESP_OK && v >= -50 && v <= 50) {  // the setter's limits: -5..+5 K
            s_st.offset = v / 10.0f;
        }
        if (nvs_get_u8(h, "sim", &b) == ESP_OK) {
            s_st.boiler_sim = b;
            s_boiler_auto = false;
            s_boiler_chosen = true;
        }
        if (nvs_get_u8(h, "simroom", &b) == ESP_OK) {
            s_st.sim_room = b;
        }
        int64_t until = 0, end;
        bool have_flag = nvs_get_u8(h, "away", &b) == ESP_OK;
        bool have_until = nvs_get_i64(h, "awayuntil", &until) == ESP_OK;
        if (away_load(have_flag, b, have_until, until, &end)) {
            s_st.away = true;
            s_st.away_until = end;
        }
        nvs_close(h);
    }
}

// Returns false when something could not be written: the caller tries again later.
static bool save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("thermo", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings not saved: %s", esp_err_to_name(err));
        return false;
    }
    LOCK();
    thermo_state_t c = s_st;
    UNLOCK();
    int bad = 0;
    bad += nvs_set_i32(h, "sp", (int32_t)lroundf(c.setpoint * 10)) != ESP_OK;
    bad += nvs_set_u8(h, "mode", c.mode) != ESP_OK;
    bad += nvs_set_i32(h, "ofs", (int32_t)lroundf(c.offset * 10)) != ESP_OK;
    if (s_boiler_chosen) {  // a simulator that only stepped in for a missing boiler is never stored
        bad += nvs_set_u8(h, "sim", c.boiler_sim) != ESP_OK;
    }
    bad += nvs_set_u8(h, "simroom", c.sim_room) != ESP_OK;
    // Key order so a power cut in between loads the old or the new state, never "away for good" (awaykeys.h)
    away_key_t order[2];
    away_save_order(c.away, order);
    for (int i = 0; i < 2; i++) {
        bad += (order[i] == AWAY_KEY_FLAG ? nvs_set_u8(h, "away", c.away)
                                          : nvs_set_i64(h, "awayuntil", c.away ? c.away_until : 0)) != ESP_OK;
    }
    bad += nvs_commit(h) != ESP_OK;
    nvs_close(h);
    if (bad) {
        ESP_LOGE(TAG, "settings not saved completely (%d writes failed); trying again in a minute", bad);
        return false;
    }
    ESP_LOGI(TAG, "settings saved (sp %.1f, mode %d)", c.setpoint, c.mode);
    return true;
}

static void mark_dirty(void)
{
    s_dirty_ms = dirty_now();
}

static void mark_scfg_dirty(void)
{
    s_scfg_dirty = true;
    s_dirty_ms = dirty_now();
}

static void mark_cfg_dirty(void)
{
    s_cfg_changed = true;
    s_cfg_dirty = true;
    s_dirty_ms = dirty_now();
}

/* ---------------------------------------------------------------- boiler */

// The bus task (ot_bus.c) talks to the boiler every second; here we only read what it learned and hand
// over the next request.
static void boiler_read(otb_status_t *b)
{
    otbus_get(b);
    LOCK();
    s_st.boiler_online = b->link == OTB_LINK_UP;
    s_st.flame = b->slave_flags & OT_S_FLAME;
    s_st.dhw_active = b->slave_flags & OT_S_DHW_ACTIVE;
    s_st.flow_c = b->flow_c;
    s_st.return_c = b->return_c;
    s_st.modulation = b->modulation;
    s_st.ot_ok = b->ok;
    s_st.ot_errors = b->timeouts + b->bad;
    s_st.tank_valid = b->valid & OTB_V_TDHW;
    s_st.tank_c = b->tank_c;
    s_st.outside_valid = b->valid & OTB_V_TOUT;  // boiler outdoor probe; HA outdoor comes with Phase 1b
    s_st.outside_c = b->outside_c;
    s_st.id56 = b->dhw_set_verify;
    s_st.press_valid = b->valid & OTB_V_PRESS;
    s_st.pressure_bar = b->pressure_bar;
    bool up = b->link == OTB_LINK_UP;
    s_st.exhaust_valid = up && (b->valid & OTB_V_TEXH);
    s_st.exhaust_c = b->exhaust_c;
    s_st.fan_valid = up && (b->valid & OTB_V_FAN);
    s_st.fan_rpm = (uint16_t)(b->fan_hz * 60);
    s_st.flame_i_valid = up && (b->valid & OTB_V_FLAME_I);
    s_st.flame_ua = b->flame_ua;
    s_st.counters_valid = b->valid & OTB_V_COUNTERS;
    memcpy(s_st.counters, b->counters, sizeof(s_st.counters));
    s_st.boiler_fault = b->link == OTB_LINK_UP && (b->slave_flags & OT_S_FAULT);
    s_st.oem_fault = b->oem_fault;
    UNLOCK();
}

/* --------------------------------------------------------------- history */

// 5-second samples for the last hour (chart: 5 min and 1 h views); RAM only.
static void fine_add(void)
{
    LOCK();
    thermo_fine_t f = {
        .t = timekeep_valid() ? (uint32_t)time(NULL) : 0,
        .room_c10 = s_st.sensor_ok ? (int16_t)lroundf(s_st.room_c * 10) : INT16_MIN,
        .target_c10 = (int16_t)lroundf(
            (s_st.mode == THERMO_MODE_HEAT && s_st.sched_src != SCHED_SRC_SUMMER ? s_st.target_c : s_cfg.frost_room_c) * 10),
        .flow_c10 = s_st.boiler_online ? (int16_t)lroundf(s_st.flow_c * 10) : INT16_MIN,
        .flags = (s_st.flame ? THERMO_FINE_FLAME : 0) | (s_st.flame && !s_st.dhw_active ? THERMO_FINE_HEAT : 0),
    };
    s_fine[s_fine_head] = f;
    s_fine_head = (s_fine_head + 1) % THERMO_FINE_LEN;
    if (s_fine_count < THERMO_FINE_LEN) {
        s_fine_count++;
    }
    UNLOCK();
}

int thermo_fine(thermo_fine_t *out, int max)
{
    LOCK();
    int n = s_fine_count < max ? s_fine_count : max;
    int start = (s_fine_head - n + THERMO_FINE_LEN) % THERMO_FINE_LEN;
    for (int i = 0; i < n && s_fine; i++) {
        out[i] = s_fine[(start + i) % THERMO_FINE_LEN];
    }
    UNLOCK();
    return s_fine ? n : 0;
}

// The last max minutes from the data log (kept in flash, so it survives restarts), oldest first.
int thermo_history(thermo_sample_t *out, int max)
{
    LOCK();
    int count = datalog_count(&s_dl);
    int n = count < max ? count : max;
    for (int i = 0; i < n; i++) {
        dl_rec_t r;
        datalog_get(&s_dl, count - n + i, &r);
        out[i] = (thermo_sample_t){
            .t = (time_t)r.t,
            .room_c10 = r.room_c100 == DL_NO_VALUE ? INT16_MIN : (int16_t)lround(r.room_c100 / 10.0),
            .humidity = r.humidity,
            .setpoint_c10 = r.target_c10,
            .demand = (r.flags & DL_F_CH_EN) != 0,
        };
    }
    UNLOCK();
    return n;
}

/* ------------------------------------------------------------------ task */

// Local calendar day as days since 1970-01-01 (for once-a-week tracking).
static int32_t local_day(const struct tm *t)
{
    int y = t->tm_year + 1900, m = t->tm_mon + 1, d = t->tm_mday;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static volatile int s_hang_s;
static volatile bool s_hang_feed;

void thermo_test_hang(int seconds, bool feed_watchdog)
{
    s_hang_feed = feed_watchdog;
    s_hang_s = seconds < 1 ? 1 : (seconds > 60 ? 60 : seconds);
}

static void thermo_task(void *arg)
{
    esp_task_wdt_add(NULL);
    bool sensor_inited = sht4x_init(board_i2c_bus()) == ESP_OK;
    TickType_t wake = xTaskGetTickCount();
    int64_t last_hist = 0;
    for (int loop = 0;; loop++) {
        esp_task_wdt_reset();
        if (s_hang_s) {  // fault test (console HANG): this task stops, as if it were stuck
            int n = s_hang_s;
            s_hang_s = 0;
            ESP_LOGW(TAG, "test: control task stops for %d s (%s the watchdog)", n, s_hang_feed ? "feeding" : "not feeding");
            for (int i = 0; i < n; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (s_hang_feed) {
                    esp_task_wdt_reset();
                }
            }
            ESP_LOGW(TAG, "test: control task continues");
            wake = xTaskGetTickCount();
        }
        int64_t now = esp_timer_get_time();

        // 1. Sensor (every 2 s)
        if (loop % 2 == 0) {
            float t, h;
            esp_err_t err = sensor_inited ? sht4x_measure(&t, &h) : ESP_ERR_INVALID_STATE;
            if (!sensor_inited) {
                sensor_inited = sht4x_init(board_i2c_bus()) == ESP_OK;
            }
            // Recovery: after 5 failed reads in a row a soft reset of the sensor, after 15 (and then every 30 s)
            // a reset of the I2C bus as well (a device holding the data line low blocks everyone on it).
            // Bit-identical readings for 5 minutes count as failed (sensor_filter.c).
            sf_action_t act;
            bool stuck_now;
            if (!sf_check(&s_sf, err == ESP_OK, t, h, &act, &stuck_now) && err == ESP_OK) {
                err = ESP_ERR_INVALID_RESPONSE;
            }
            if (act == SF_ACT_SOFT_RESET && sensor_inited) {
                ESP_LOGW(TAG, "SHT40: 5 failed reads, soft reset: %s", esp_err_to_name(sht4x_reset()));
            } else if (act == SF_ACT_BUS_RESET && board_i2c_bus()) {
                esp_err_t br = i2c_master_bus_reset(board_i2c_bus());
                ESP_LOGW(TAG, "SHT40: %d failed reads, I2C bus reset: %s", s_sf.fails_in_row, esp_err_to_name(br));
                if (sensor_inited) {
                    sht4x_reset();
                }
            }
            if (stuck_now) {
                ESP_LOGE(TAG, "SHT40 repeats the same reading (%.2f C, %.1f %%) for %d s: treated as failed", t, h,
                         SF_STUCK_N * 2);
                sht4x_reset();
            }
            bool sht_read = err == ESP_OK && !stuck_now;  // the real sensor answered (before the simulator)
            float sht_t = t, sht_h = h;
            // Bench testing: the simulated house replaces the SHT40 temperature (humidity stays real).
            float sim_t;
            LOCK();
            bool use_sim_room = s_st.sim_room && s_st.boiler_sim;
            UNLOCK();
            if (use_sim_room && otbus_sim_room(&sim_t)) {
                t = sim_t;
                if (err != ESP_OK) {
                    h = 45;
                    err = ESP_OK;
                }
            }
            LOCK();
            if (!sf_accept(&s_sf, now, err == ESP_OK, t + s_st.offset, h)) {
                if (s_sf.errors < 5 || s_sf.errors % 15 == 0) {
                    ESP_LOGW(TAG, "SHT40 read failed (%s), %lu errors, last good %d s ago",
                             err == ESP_OK ? "implausible value" : esp_err_to_name(err), (unsigned long)s_sf.errors,
                             s_sf.age_s);
                }
            }
            s_st.room_raw = s_sf.raw;
            s_st.room_c = s_sf.room_c;
            s_st.humidity = s_sf.humidity;
            s_st.room_shown = s_sf.room_shown;
            s_st.humidity_shown = s_sf.humidity_shown;
            s_st.sensor_errors = s_sf.errors;
            s_st.sensor_ok = s_sf.ok;
            s_st.sensor_age_s = s_sf.age_s;
            UNLOCK();
            if (s_sf.back_s < 0 && s_sf.back_bad) {  // never read since the start, now it does
                ESP_LOGI(TAG, "SHT40 answering after %d failed reads since the start, %.2f C", s_sf.back_bad,
                         s_sf.room_c);
            } else if (s_sf.back_bad >= 5) {  // shorter glitches are only counted in the errors
                ESP_LOGI(TAG, "SHT40 back after %d s (%d failed reads), %.2f C", s_sf.back_s, s_sf.back_bad,
                         s_sf.room_c);
            }
            if (sht_read) {  // clears a failed boot test (status bar); returns at once when it passed
                selftest_sensor_found(sht_t, sht_h);
            }
        }

        // 2. Control (control.c), on the boiler state the bus task last saw
        otb_status_t b;
        boiler_read(&b);
        bool clock_ok = timekeep_valid();
        time_t wall = time(NULL);
        struct tm lt = {0};
        localtime_r(&wall, &lt);
        LOCK();
        // Once the clock is known: take the PI long-term correction from the last minute saved before the
        // restart, if that minute is recent (an update or watchdog restart, not a week switched off).
        static bool i_restored;
        if (clock_ok && !i_restored) {
            i_restored = true;
            dl_rec_t last;
            if (histstore_last_rec(&last) && last.t > 0 && wall - last.t >= 0 && wall - last.t < 2 * 3600 &&
                last.i_c10 != DL_NO_VALUE) {
                ctrl_set_integral(&s_ctrl, last.i_c10 / 10.0f);
                ESP_LOGI(TAG, "PI correction restored: %.1f K (saved %lld s ago)", last.i_c10 / 10.0f,
                         (long long)(wall - last.t));
            }
            // The starts-per-hour limit: CH burns of the last hour from the flash history (a burn cut short by
            // the restart itself is not in it)
            int nb = datalog_burn_count(&s_dl), back = 0;
            for (int k = 0; k < nb; k++) {
                dl_burn_t bu;
                if (datalog_get_burn(&s_dl, k, &bu) && !bu.dhw && bu.t > 0 && wall - (time_t)bu.t >= 0 &&
                    wall - (time_t)bu.t < 3600) {
                    ctrl_note_start(&s_ctrl, now / 1e6 - (double)(wall - (time_t)bu.t));
                    back++;
                }
            }
            if (back) {
                ESP_LOGI(TAG, "%d burner starts of the last hour restored (starts-per-hour limit)", back);
            }
        }
        if (s_boiler_auto) {
            // Only an answer on the real bus counts: the bus task starts with the simulator, whose link is up
            otb_status_t rb;
            bool rb_sim;
            otbus_get_mode(&rb, &rb_sim);
            if (!rb_sim && rb.link == OTB_LINK_UP) {
                s_boiler_auto = false;
                s_boiler_chosen = true;
                s_dirty_ms = dirty_now();  // save the choice
                ESP_LOGI(TAG, "boiler found on the OpenTherm bus: using the real boiler");
            } else if (loop >= BOILER_DETECT_S) {
                s_boiler_auto = false;
                s_st.boiler_sim = true;
                ESP_LOGW(TAG, "no boiler answered in %d s: simulated boiler (not saved; set it in Settings)",
                         BOILER_DETECT_S);
            }
        }
        if (s_cfg_changed) {
            s_cfg_changed = false;
            ctrl_set_config(&s_ctrl, &s_cfg);
        }
        bool away_before = s_sched.away;
        sched_in_t si = {
            .now_s = now / 1e6,
            .clock_ok = clock_ok,
            .wday = (lt.tm_wday + 6) % 7,
            .min_of_day = lt.tm_hour * 60 + lt.tm_min,
            .day = local_day(&lt),
            .epoch = (int64_t)wall,
            .comfort_c = s_st.setpoint,
            .tank_valid = (b.valid & OTB_V_TDHW) != 0,
            .tank_c = b.tank_c,
            .outside_valid = (b.valid & OTB_V_TOUT) != 0,
            .outside_c = b.outside_c,
            .flame = (b.slave_flags & OT_S_FLAME) != 0,
            .dhw_active = (b.slave_flags & OT_S_DHW_ACTIVE) != 0,
            .dhw_set_valid = (b.valid & OTB_V_DHW_SET) != 0,
            .dhw_set_read = b.dhw_set_c,
            .id56 = b.dhw_set_verify,
        };
        const sched_out_t *so = sched_step(&s_sched, &si);
        if (s_sched.away != away_before) {
            mark_dirty();  // holiday ended: keep the saved state in step
        }
        s_st.target_c = so->target_c;
        s_st.sched_src = so->src;
        s_st.next_change_min = so->next_change_min;
        s_st.next_level = so->next_level;
        s_st.boost_left_s = so->boost_left_s;
        s_st.away = so->away;
        s_st.away_until = so->away_until;
        s_st.override_until = so->override_until;
        s_st.summer = so->summer;
        s_st.outside_mean_valid = so->outside_mean_valid;
        s_st.outside_mean_c = so->outside_mean_c;
        s_st.dhw_enable = so->dhw_enable;
        s_st.dhw_reason = so->dhw_reason;
        s_st.dhw_boost_left_s = so->dhw_boost_left_s;
        s_st.dhw_set = so->dhw_set;
        s_st.pasteur_running = so->pasteur_running;
        s_st.pasteur_result = so->pasteur_result;
        s_st.pasteur_last = so->pasteur_last;
        ctrl_in_t in = {
            .now_s = now / 1e6,
            .heat_mode = s_st.mode == THERMO_MODE_HEAT && so->ch_allowed,
            .target_c = so->target_c,
            .room_ok = s_st.sensor_ok,
            .room_c = s_st.room_c,
            .boiler_ok = b.link == OTB_LINK_UP,
            .flame = (b.slave_flags & OT_S_FLAME) != 0,
            .dhw_active = (b.slave_flags & OT_S_DHW_ACTIVE) != 0,
            .flow_valid = (b.valid & OTB_V_FLOW) != 0,
            .flow_c = b.flow_c,
            .mod_valid = (b.valid & OTB_V_MOD) != 0,
            .modulation = b.modulation,
        };
        const ctrl_out_t *o = ctrl_step(&s_ctrl, &in);
        s_st.demand = o->ch_enable;
        s_st.flow_setpoint = o->tset;
        s_st.ctrl_state = o->state;
        s_st.req_c = o->req_c;
        s_st.pi_p = o->p;
        s_st.pi_i = o->i;
        s_st.duty = o->duty;
        s_st.starts_last_h = o->starts_last_h;
        // Data log and KPIs (every second; a record per minute)
        float cap = (b.valid & OTB_V_CAPS) ? b.max_cap_kw : 0;
        dl_sample_t ds = {
            .epoch = clock_ok ? (int64_t)wall : 0,
            .sensor_ok = s_st.sensor_ok, .room_c = s_st.room_c, .humidity = s_st.humidity,
            .target_c = so->target_c, .setpoint_c = s_st.setpoint,
            .heat_mode = in.heat_mode, .ch_allowed = so->ch_allowed,
            .boiler_up = in.boiler_ok, .sim = s_st.boiler_sim,
            .flame = in.flame, .dhw_active = in.dhw_active, .fault = s_st.boiler_fault,
            .ch_enable = o->ch_enable, .dhw_enable = so->dhw_enable,
            .tset_c = o->tset, .req_c = o->req_c, .p = o->p, .i = o->i,
            .flow_valid = in.flow_valid, .ret_valid = (b.valid & OTB_V_RET) != 0, .mod_valid = in.mod_valid,
            .press_valid = s_st.press_valid, .tank_valid = si.tank_valid, .out_valid = si.outside_valid,
            .flow_c = b.flow_c, .return_c = b.return_c, .modulation = b.modulation, .pressure_bar = b.pressure_bar,
            .tank_c = b.tank_c, .outside_c = b.outside_c,
            .ctrl_state = o->state, .sched_src = so->src, .dhw_reason = so->dhw_reason,
            .rssi = (int8_t)s_rssi, .ot_errors = b.timeouts + b.bad,
            .cap_kw = cap, .min_kw = cap * ((b.valid & OTB_V_CAPS) ? b.min_mod_pct : 0) / 100.0f,
            .svc = s_svc.active,
        };
        bool kpi_due = datalog_tick(&s_dl, &ds) || loop == 0;  // computed after the lock is released, see below
        s_st.gas_kwh_total = s_dl.total_kwh;
        s_st.ch_starts_total = s_dl.total_ch_starts;
        s_st.dhw_starts_total = s_dl.total_dhw_starts;
        s_st.burner_h_total = s_dl.total_burner_s / 3600.0;

        // Alerts
        if (s_acfg_changed) {
            s_acfg_changed = false;
            alerts_set_config(&s_alerts, &s_acfg);
        }
        alert_in_t ai = {
            .now_s = now / 1e6,
            .boiler_fault = s_st.boiler_fault,
            .oem_fault = b.oem_fault,
            .fault_valid = (b.valid & OTB_V_FAULT) != 0,
            .press_valid = s_st.press_valid,
            .pressure_bar = b.pressure_bar,
            .link_up = b.link == OTB_LINK_UP,
            .link_known = b.link != OTB_LINK_UNKNOWN,
            .sensor_ok = s_st.sensor_ok,
            .ch_starts_1h = s_kpi1.ch_starts,
            .median_burn_1h_min = s_kpi1.median_burn_min,
            .ch_burns_1h = s_kpi1.ch_burns,
            .heating_allowed = in.heat_mode,
            .room_c = s_st.room_c,
            .target_c = so->target_c,
            .pasteur_result = so->pasteur_result,
            .clock_ok = clock_ok,
        };
        const alert_out_t *ao = alerts_step(&s_alerts, &ai);
        uint16_t bits = 0;
        for (int a = 0; a < ALERT_COUNT; a++) {
            bits |= ao->active[a] ? 1u << a : 0;
        }
        if (bits != s_st.alerts) {
            ESP_LOGW(TAG, "alerts %04x -> %04x: %s", s_st.alerts, bits, ao->text[0] ? ao->text : "none");
        }
        s_st.alerts = bits;
        s_st.alert_count = (uint8_t)ao->count;
        strlcpy(s_st.alert_text, ao->text, sizeof(s_st.alert_text));

        bool sim = s_st.boiler_sim;
        s_st.loops++;
        otb_cmd_t cmd = {.ch_enable = o->ch_enable, .dhw_enable = so->dhw_enable, .tset = o->tset,
                         .dhw_set = so->dhw_set};

        // Service mode: a running routine replaces the request (service.c)
        otb_manual_t man;
        otbus_manual_get(&man);
        svc_in_t vi = {
            .now_s = now / 1e6, .epoch = clock_ok ? (int64_t)wall : 0, .sim = s_st.boiler_sim, .bus = &b, .manual = &man,
            .room_ok = s_st.sensor_ok, .room_c = s_st.room_c, .frost_c = s_cfg.frost_room_c,
            .flow_max_c = s_cfg.flow_max_c,
        };
        otbus_get_flags(&vi.flags);
        const svc_out_t *vo = svc_step(&s_svc, &vi);
        if (vo->override) {
            cmd = vo->cmd;
            cmd.dhw_set = 0;
            s_st.demand = cmd.ch_enable;
            s_st.flow_setpoint = cmd.tset;
        }
        s_st.svc_active = vo->active;
        s_st.svc_running = vo->running;
        s_st.svc_routine = vo->routine;
        s_st.svc_elapsed_s = vo->elapsed_s;
        s_st.svc_idle_left_s = vo->idle_left_s;
        strlcpy(s_st.svc_step, vo->step, sizeof(s_st.svc_step));
        bool svc_manual = vo->manual, svc_flags = vo->set_flags, svc_silent = vo->silent;
        uint8_t mt = vo->m_type, mi = vo->m_id;
        uint16_t md = vo->m_data;
        otb_flags_t sf = vo->flags;
        bool svc_save = vo->record_changed;
        UNLOCK();
        if (kpi_due) {
            // The KPI passes read up to a day of records. Only this task changes s_dl, so they run without the
            // lock (the screen and the web page never wait for them); the results are copied in under it.
            static dl_kpi_t k1, k24;
            datalog_kpi(&s_dl, 1, &k1);
            datalog_kpi(&s_dl, 24, &k24);
            LOCK();
            s_kpi1 = k1;
            s_kpi24 = k24;
            UNLOCK();
        }
        histstore_sync(&s_dl);  // new minute / burn / hour to flash (only this task changes s_dl)

        // 3. Boiler
        otbus_set_simulated(sim);
        if (svc_flags) {
            otbus_set_flags(&sf, false);  // RAM only: a routine's temporary flags never reach NVS
        }
        otbus_set_silent(svc_silent);
        if (svc_manual) {
            otbus_manual(mt, mi, md);
        }
        otbus_set_cmd(&cmd);
        if (svc_save) {
            save_svc(false, true);
        }

        // 4. Fine history (every 5 s) and debounced save
        if (now - last_hist >= 5LL * 1000000 || last_hist == 0) {
            last_hist = now;
            fine_add();
        }
        uint32_t dirty = s_dirty_ms, now_ms = (uint32_t)(now / 1000);
        if (dirty && (int32_t)(now_ms - dirty) > SAVE_DELAY_MS) {
            s_dirty_ms = 0;
            if (!save()) {
                s_dirty_ms = (now_ms + 55000) | 1;  // SAVE_DELAY_MS after this = one minute from now
            }
            if (s_cfg_dirty) {
                s_cfg_dirty = false;
                save_ctrl_config();
            }
            if (s_scfg_dirty) {
                s_scfg_dirty = false;
                save_sched_config();
            }
            if (s_acfg_dirty) {
                s_acfg_dirty = false;
                save_alert_config();
            }
            if (s_svccfg_dirty) {
                s_svccfg_dirty = false;
                save_svc(true, false);
            }
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
    sf_init(&s_sf, THERMO_SENSOR_STALE_S);
    s_fine = heap_caps_calloc(THERMO_FINE_LEN, sizeof(thermo_fine_t), MALLOC_CAP_SPIRAM);
    load();
    load_ctrl_config();
    load_sched_config();
    load_alert_config();
    load_svc();
    alerts_init(&s_alerts, &s_acfg);
    dl_rec_t *recs = heap_caps_calloc(LOG_RECS, sizeof(dl_rec_t), MALLOC_CAP_SPIRAM);
    dl_burn_t *burns = heap_caps_calloc(LOG_BURNS, sizeof(dl_burn_t), MALLOC_CAP_SPIRAM);
    dl_hour_t *hours = heap_caps_calloc(LOG_HOURS, sizeof(dl_hour_t), MALLOC_CAP_SPIRAM);
    datalog_init(&s_dl, recs, recs ? LOG_RECS : 0, burns, burns ? LOG_BURNS : 0, hours, hours ? LOG_HOURS : 0);
    if (histstore_init()) {
        histstore_restore(&s_dl);  // the last 24 h and 35 days of hours survive restarts
    }
    s_st.hysteresis = s_cfg.onoff_hyst_k;
    ctrl_init(&s_ctrl, &s_cfg);
    sched_init(&s_sched, &s_scfg);
    if (s_st.away && !sched_away(&s_sched, true, s_st.away_until)) {
        s_st.away = false;  // feature switched off since
    }
    s_st.target_c = s_st.setpoint;
    ESP_LOGI(TAG, "start: setpoint %.1f, mode %s, offset %.1f, boiler %s", s_st.setpoint,
             s_st.mode ? "heat" : "off", s_st.offset, s_st.boiler_sim ? "simulated" : "real");
    // Highest application priority, pinned to core 1 (the UI runs on core 0).
    xTaskCreatePinnedToCore(thermo_task, "thermo", 8192, NULL, 20, NULL, 1);  // KPI pass needs ~1 KB
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
    sf_shift(&s_sf, d);
    s_st.room_c = s_sf.room_c;
    s_st.room_raw = s_sf.raw;
    s_st.room_shown = s_sf.room_shown;
    s_st.offset = c;
    UNLOCK();
    mark_dirty();
}

void thermo_set_hysteresis(float c)
{
    c = fminf(fmaxf(roundf(c * 20) / 20, 0.05f), 1.0f);
    LOCK();
    s_st.hysteresis = c;
    s_cfg.onoff_hyst_k = c;  // used by the on/off strategy
    UNLOCK();
    mark_cfg_dirty();
}

void thermo_get_ctrl_config(ctrl_config_t *out)
{
    LOCK();
    *out = s_cfg;
    UNLOCK();
}

void thermo_set_ctrl_config(const ctrl_config_t *c)
{
    ctrl_config_t n = *c;
    ctrl_sanitize(&n);
    LOCK();
    s_cfg = n;
    s_st.hysteresis = n.onoff_hyst_k;
    UNLOCK();
    mark_cfg_dirty();
}

void thermo_set_sim_room(bool on)
{
    LOCK();
    s_st.sim_room = on;
    UNLOCK();
    mark_dirty();
}

int thermo_forget_boiler_choice(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("thermo", NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_erase_key(h, "sim");
        if (e == ESP_ERR_NVS_NOT_FOUND) {
            e = ESP_OK;
        }
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    return e;
}

void thermo_set_boiler_sim(bool sim)
{
    LOCK();
    s_boiler_auto = false;
    s_boiler_chosen = true;
    s_st.boiler_sim = sim;
    s_st.boiler_online = false;
    UNLOCK();
    mark_dirty();
}

void thermo_get_sched_config(sched_config_t *out)
{
    LOCK();
    *out = s_scfg;
    UNLOCK();
}

void thermo_set_sched_config(const sched_config_t *c)
{
    sched_config_t n = *c;
    sched_sanitize(&n);
    LOCK();
    s_scfg = n;
    sched_set_config(&s_sched, &n);  // at once, so a user action right after sees the new switches
    UNLOCK();
    mark_scfg_dirty();
}

static double now_s(void)
{
    return esp_timer_get_time() / 1e6;
}

void thermo_set_target(float c)
{
    c = roundf(c * 2) / 2;
    c = fminf(fmaxf(c, THERMO_SP_MIN), THERMO_SP_MAX);
    LOCK();
    sched_boost(&s_sched, now_s(), false);
    bool was_away = s_sched.away;
    if (was_away) {
        sched_away(&s_sched, false, 0);
        s_st.away = false;
        s_st.away_until = 0;
    }
    bool program = s_sched.cfg.prog_on && timekeep_valid();
    if (program) {
        sched_override(&s_sched, true, c);
        s_st.target_c = c;  // shown at once; the next step confirms it
        s_st.sched_src = SCHED_SRC_MANUAL;
    }
    UNLOCK();
    if (!program) {
        thermo_set_setpoint(c);
    } else if (was_away) {
        mark_dirty();
    }
}

void thermo_adjust_target(float delta)
{
    LOCK();
    float c = s_st.target_c;
    UNLOCK();
    thermo_set_target(c + delta);
}

void thermo_resume_program(void)
{
    LOCK();
    sched_override(&s_sched, false, 0);
    UNLOCK();
}

bool thermo_boost(bool on)
{
    LOCK();
    bool ok = sched_boost(&s_sched, now_s(), on);
    UNLOCK();
    return ok;
}

bool thermo_dhw_boost(bool on)
{
    LOCK();
    bool ok = sched_dhw_boost(&s_sched, now_s(), on);
    UNLOCK();
    return ok;
}

bool thermo_away(bool on, int64_t until)
{
    LOCK();
    bool ok = sched_away(&s_sched, on, until);
    if (ok) {
        s_st.away = on;
        s_st.away_until = on ? until : 0;
    }
    UNLOCK();
    if (ok) {
        mark_dirty();
    }
    return ok;
}

bool thermo_pasteurise(bool on)
{
    LOCK();
    bool ok = sched_pasteur_now(&s_sched, on);
    UNLOCK();
    return ok;
}

void thermo_get_alert_config(alert_config_t *out)
{
    LOCK();
    *out = s_acfg;
    UNLOCK();
}

void thermo_set_alert_config(const alert_config_t *c)
{
    alert_config_t n = *c;
    alert_sanitize(&n);
    LOCK();
    s_acfg = n;
    s_acfg_changed = true;
    s_acfg_dirty = true;
    UNLOCK();
    s_dirty_ms = dirty_now();
}

void thermo_get_kpi(dl_kpi_t *h1, dl_kpi_t *h24)
{
    LOCK();
    if (h1) {
        *h1 = s_kpi1;
    }
    if (h24) {
        *h24 = s_kpi24;
    }
    UNLOCK();
}

int thermo_log_count(void)
{
    LOCK();
    int n = datalog_count(&s_dl);
    UNLOCK();
    return n;
}

bool thermo_log_get(int i, dl_rec_t *out)
{
    LOCK();
    bool ok = datalog_get(&s_dl, i, out);
    UNLOCK();
    return ok;
}

int thermo_hour_count(void)
{
    LOCK();
    int n = datalog_hour_count(&s_dl);
    UNLOCK();
    return n;
}

bool thermo_hour_get(int i, dl_hour_t *out)
{
    LOCK();
    bool ok = datalog_get_hour(&s_dl, i, out);
    UNLOCK();
    return ok;
}

void thermo_set_rssi(int rssi)
{
    s_rssi = rssi;
}

static double now_mono(void)
{
    return esp_timer_get_time() / 1e6;
}

bool thermo_svc_enter(bool on)
{
    LOCK();
    if (on) {
        svc_enter(&s_svc, now_mono());
    } else {
        svc_exit(&s_svc);
    }
    UNLOCK();
    ESP_LOGW(TAG, "service mode %s", on ? "on" : "off");
    return true;
}

bool thermo_svc_start(svc_routine_t r, bool confirmed, char *why, size_t size)
{
    LOCK();
    bool ok = svc_start(&s_svc, r, confirmed, now_mono(), why, size);
    UNLOCK();
    if (ok) {
        ESP_LOGW(TAG, "service routine %s started", svc_routine_name(r));
    }
    return ok;
}

void thermo_svc_cancel(void)
{
    LOCK();
    svc_cancel(&s_svc);
    UNLOCK();
}

bool thermo_svc_manual(bool write, uint8_t id, uint16_t data, char *why, size_t size)
{
    LOCK();
    bool ok = svc_manual(&s_svc, write, id, data, now_mono(), why, size);
    UNLOCK();
    return ok;
}

bool thermo_svc_apply(svc_routine_t r, char *msg, size_t size)
{
    svc_apply_t a;
    LOCK();
    bool ok = svc_recommend(&s_svc.rec, r, &a);
    UNLOCK();
    if (!ok) {
        snprintf(msg, size, "nothing to apply (run %s first)", svc_routine_name(r));
        return false;
    }
    otb_flags_t f;
    otbus_get_flags(&f);
    if (a.set_ca) {
        f.status_ca = a.ca;
    }
    if (a.set_mirror) {
        f.mirror_57 = a.mirror;
    }
    if (a.set_explicit_off) {
        f.explicit_off = a.explicit_off;
    }
    if (a.set_ca || a.set_mirror || a.set_explicit_off) {
        otbus_set_flags(&f, true);
    }
    if (a.set_flow_min) {
        ctrl_config_t c;
        thermo_get_ctrl_config(&c);
        c.flow_min_c = a.flow_min_c;
        thermo_set_ctrl_config(&c);
    }
    snprintf(msg, size, "applied: ca=%d mirror57=%d explicitoff=%d%s", f.status_ca, f.mirror_57, f.explicit_off,
             a.set_flow_min ? ", flow_min_c set" : "");
    return true;
}

void thermo_svc_get(svc_record_t *rec, char *manual_text, size_t size)
{
    LOCK();
    if (rec) {
        *rec = s_svc.rec;
    }
    if (manual_text) {
        strlcpy(manual_text, s_svc.out.manual_text, size);
    }
    UNLOCK();
}

void thermo_get_svc_config(svc_config_t *out)
{
    LOCK();
    *out = s_svc.cfg;
    UNLOCK();
}

void thermo_set_svc_config(const svc_config_t *c)
{
    LOCK();
    svc_set_config(&s_svc, c);
    s_svccfg_dirty = true;
    UNLOCK();
    s_dirty_ms = dirty_now();
}
