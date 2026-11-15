// Thermostat control task.
// Every second: read the SHT40 (every 2nd loop), run the control law (control.c) on the room and the boiler
// state, and hand the request to the OpenTherm bus task (ot_bus.c).
// Sensor lost (no reading for 30 s), frost floor and every other feature: see control.h and ctrl_config_t.
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
#include "control.h"
#include "timekeep.h"

static const char *TAG = "thermo";

#define LOOP_MS          1000
#define SENSOR_STALE_US  ((int64_t)THERMO_SENSOR_STALE_S * 1000000)
#define SAVE_DELAY_US    (5LL * 1000000)
#define SMOOTH_ALPHA     (2.0f / 30.0f)  // reading every 2 s -> ~30 s exponential average
#define SHOW_BAND_C      0.08f           // shown temperature moves only past this from the smoothed value
#define SHOW_BAND_RH     0.8f

static SemaphoreHandle_t s_lock;
static thermo_state_t s_st;
static int64_t s_last_reading_us;
static int64_t s_dirty_since_us;  // 0 = nothing to save
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
static dl_kpi_t s_kpi1, s_kpi24;
static volatile int s_rssi;
#define LOG_RECS (7 * 1440)
#define LOG_BURNS 4096
static thermo_sample_t *s_hist;   // PSRAM ring
static int s_hist_head, s_hist_count;

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
        if (nvs_get_u8(h, "sim", &b) == ESP_OK) {
            s_st.boiler_sim = b;
        }
        if (nvs_get_u8(h, "simroom", &b) == ESP_OK) {
            s_st.sim_room = b;
        }
        int64_t until;
        if (nvs_get_u8(h, "away", &b) == ESP_OK && b && nvs_get_i64(h, "awayuntil", &until) == ESP_OK) {
            s_st.away = true;
            s_st.away_until = until;
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
    nvs_set_u8(h, "sim", c.boiler_sim);
    nvs_set_u8(h, "simroom", c.sim_room);
    nvs_set_u8(h, "away", c.away);
    nvs_set_i64(h, "awayuntil", c.away_until);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "settings saved (sp %.1f, mode %d)", c.setpoint, c.mode);
}

static void mark_dirty(void)
{
    s_dirty_since_us = esp_timer_get_time();
}

static void mark_scfg_dirty(void)
{
    s_scfg_dirty = true;
    s_dirty_since_us = esp_timer_get_time();
}

static void mark_cfg_dirty(void)
{
    s_cfg_changed = true;
    s_cfg_dirty = true;
    s_dirty_since_us = esp_timer_get_time();
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
    s_st.boiler_fault = b->link == OTB_LINK_UP && (b->slave_flags & OT_S_FAULT);
    s_st.oem_fault = b->oem_fault;
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
        .setpoint_c10 = (int16_t)lroundf(
            (s_st.mode == THERMO_MODE_HEAT && s_st.sched_src != SCHED_SRC_SUMMER ? s_st.target_c : s_cfg.frost_room_c) * 10),
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

        // 2. Control (control.c), on the boiler state the bus task last saw
        otb_status_t b;
        boiler_read(&b);
        bool clock_ok = timekeep_valid();
        time_t wall = time(NULL);
        struct tm lt = {0};
        localtime_r(&wall, &lt);
        LOCK();
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
        };
        if (datalog_tick(&s_dl, &ds) || loop == 0) {
            datalog_kpi(&s_dl, 1, &s_kpi1);
            datalog_kpi(&s_dl, 24, &s_kpi24);
        }
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
        UNLOCK();

        // 3. Boiler
        otbus_set_simulated(sim);
        otbus_set_cmd(&cmd);

        // 4. History (1 / min) and debounced save
        if (now - last_hist >= 60LL * 1000000 || last_hist == 0) {
            last_hist = now;
            history_add();
        }
        if (s_dirty_since_us && now - s_dirty_since_us > SAVE_DELAY_US) {
            s_dirty_since_us = 0;
            save();
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
    load_ctrl_config();
    load_sched_config();
    load_alert_config();
    alerts_init(&s_alerts, &s_acfg);
    dl_rec_t *recs = heap_caps_calloc(LOG_RECS, sizeof(dl_rec_t), MALLOC_CAP_SPIRAM);
    dl_burn_t *burns = heap_caps_calloc(LOG_BURNS, sizeof(dl_burn_t), MALLOC_CAP_SPIRAM);
    datalog_init(&s_dl, recs, recs ? LOG_RECS : 0, burns, burns ? LOG_BURNS : 0);
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

void thermo_set_boiler_sim(bool sim)
{
    LOCK();
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
    s_dirty_since_us = esp_timer_get_time();
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

void thermo_set_rssi(int rssi)
{
    s_rssi = rssi;
}
