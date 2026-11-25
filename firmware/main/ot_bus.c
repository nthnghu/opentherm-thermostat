#include "ot_bus.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "opentherm.h"
#include "ot_sim.h"
#include "timekeep.h"
#include "nvs.h"

static const char *TAG = "otbus";

#define CYCLE_MS     1000
#define GAP_MS       100   // at least this long between the end of a reply and the next request
#define BUDGET_MS    650   // no new frame after this much of the cycle (a frame can take ~875 ms)

static SemaphoreHandle_t s_lock;   // s_cmd, s_flags, s_pub, s_support
static SemaphoreHandle_t s_sim_lock;
static otb_core_t s_core;          // owned by the task
static otb_cmd_t s_cmd;
static int64_t s_cmd_at_us;        // when the controller last handed over a command (under s_lock)
static bool s_cmd_stale;           // bus task only
static otb_flags_t s_flags;
static otb_status_t s_pub;
static bool s_pub_sim = true;      // s_pub comes from a cycle with the simulated boiler
static uint8_t s_support[128];
static volatile bool s_sim = true;
static ot_sim_t s_bsim;
static bool s_sim_inited;
static int64_t s_cycle_start_us, s_last_frame_end_us;
static otb_manual_t s_manual_req, s_manual_pub;  // request (pending) / last state of the core's manual frame
static bool s_silent;

static otbus_frame_t *s_ring;
static uint32_t s_ring_head, s_ring_count;
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static void capture(uint32_t req, uint32_t resp, uint16_t lat, otb_xfer_result_t r, bool sim)
{
    if (!s_ring) {
        return;
    }
    otbus_frame_t f = {.t_ms = (uint32_t)(esp_timer_get_time() / 1000), .req = req, .resp = resp,
                       .latency_ms = lat, .result = (uint8_t)r, .sim = sim};
    portENTER_CRITICAL(&s_ring_mux);
    s_ring[s_ring_head] = f;
    s_ring_head = (s_ring_head + 1) % OTBUS_FRAME_RING;
    if (s_ring_count < OTBUS_FRAME_RING) {
        s_ring_count++;
    }
    portEXIT_CRITICAL(&s_ring_mux);
}

int otbus_frames(otbus_frame_t *out, int max)
{
    if (!s_ring) {
        return 0;
    }
    portENTER_CRITICAL(&s_ring_mux);
    int n = (int)s_ring_count < max ? (int)s_ring_count : max;
    uint32_t start = (s_ring_head + OTBUS_FRAME_RING - n) % OTBUS_FRAME_RING;
    for (int i = 0; i < n; i++) {
        out[i] = s_ring[(start + i) % OTBUS_FRAME_RING];
    }
    portEXIT_CRITICAL(&s_ring_mux);
    return n;
}

// Held while a frame is on the wire: a flash write pauses the cache, and the OT interrupts are not cache-safe.
static SemaphoreHandle_t s_frame_lock;

bool otbus_quiet_begin(uint32_t timeout_ms)
{
    return !s_frame_lock || xSemaphoreTake(s_frame_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void otbus_quiet_end(void)
{
    if (s_frame_lock) {
        xSemaphoreGive(s_frame_lock);
    }
}

static volatile bool s_paused;  // console "OT line": no frames while the line is held by hand

void otbus_pause(bool on)
{
    s_paused = on;
}

static otb_xfer_result_t xfer_real(void *ctx, uint32_t req, uint32_t *resp, uint16_t *lat)
{
    if (s_paused) {
        return OTB_XFER_SKIP;
    }
    int64_t now = esp_timer_get_time();
    if (now - s_cycle_start_us > BUDGET_MS * 1000LL) {
        return OTB_XFER_SKIP;
    }
    int64_t gap_left = s_last_frame_end_us + GAP_MS * 1000LL - now;
    if (gap_left > 0) {
        vTaskDelay(pdMS_TO_TICKS((gap_left + 999) / 1000));
    }
    uint32_t r = 0;
    uint16_t l = 0;
    if (s_frame_lock) {
        xSemaphoreTake(s_frame_lock, portMAX_DELAY);
    }
    esp_err_t err = ot_transfer(req, &r, &l);
    if (s_frame_lock) {
        xSemaphoreGive(s_frame_lock);
    }
    s_last_frame_end_us = esp_timer_get_time();
    otb_xfer_result_t res = err == ESP_OK ? OTB_XFER_OK : (err == ESP_ERR_TIMEOUT ? OTB_XFER_TIMEOUT : OTB_XFER_BAD);
    capture(req, r, l, res, false);
    *resp = r;
    *lat = l;
    return res;
}

static otb_xfer_result_t xfer_sim(void *ctx, uint32_t req, uint32_t *resp, uint16_t *lat)
{
    uint32_t r = 0;
    bool ok = ot_sim_respond(&s_bsim, req, &r);
    otb_xfer_result_t res = ok ? OTB_XFER_OK : OTB_XFER_TIMEOUT;
    *lat = ok ? 40 : 0;
    capture(req, r, *lat, res, true);
    *resp = r;
    return res;
}

static void sim_init_locked(void)
{
    float tod = 8;
    if (timekeep_valid()) {
        time_t t = time(NULL);
        struct tm lt;
        localtime_r(&t, &lt);
        tod = lt.tm_hour + lt.tm_min / 60.0f;
    }
    ot_sim_init(&s_bsim, 20.0f, tod);
    s_sim_inited = true;
}

static bool s_core_sim = true;  // which boiler s_core has been talking to (bus task only)

static void bus_task(void *arg)
{
    esp_task_wdt_add(NULL);
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        esp_task_wdt_reset();
        s_cycle_start_us = esp_timer_get_time();
        LOCK();
        s_core.cmd = s_cmd;
        int64_t cmd_age_ms = s_cmd_at_us ? (s_cycle_start_us - s_cmd_at_us) / 1000 : 0;  // 0 = before the first one
        s_core.flags = s_flags;
        s_core.silent = s_silent;
        if (s_manual_req.pending && !s_core.manual.pending) {
            otb_manual_request(&s_core, s_manual_req.type, s_manual_req.id, s_manual_req.data);
            s_manual_req.pending = false;
        }
        UNLOCK();
        bool stale = otb_cmd_guard(&s_core.cmd, cmd_age_ms);
        if (stale != s_cmd_stale) {
            s_cmd_stale = stale;
            if (stale) {
                ESP_LOGE(TAG, "no command from the controller for %lld s: heating off", cmd_age_ms / 1000);
            } else {
                ESP_LOGW(TAG, "controller commands are back");
            }
        }
        int64_t now_ms = s_cycle_start_us / 1000;
        bool sim = s_sim;
        if (sim != s_core_sim) {
            // Simulated <-> real: nothing the other boiler said (link up, values, IDs) may count for this one
            otb_new_boiler(&s_core);
            s_core_sim = sim;
        }
        if (sim) {
            xSemaphoreTake(s_sim_lock, portMAX_DELAY);
            if (!s_sim_inited) {
                sim_init_locked();
            }
            otb_cycle(&s_core, now_ms, xfer_sim, NULL);
            ot_sim_tick(&s_bsim, CYCLE_MS / 1000.0f);
            xSemaphoreGive(s_sim_lock);
        } else {
            otb_cycle(&s_core, now_ms, xfer_real, NULL);
        }
        LOCK();
        s_pub = s_core.st;
        s_pub_sim = sim;
        s_manual_pub = s_core.manual;
        s_manual_pub.pending |= s_manual_req.pending;
        memcpy(s_support, s_core.support, sizeof(s_support));
        UNLOCK();
        if (xTaskDelayUntil(&wake, pdMS_TO_TICKS(CYCLE_MS)) == pdFALSE) {
            wake = xTaskGetTickCount();  // overran (slow boiler); don't try to catch up
        }
    }
}

void otbus_start(void)
{
    if (s_lock) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    s_sim_lock = xSemaphoreCreateMutex();
    s_frame_lock = xSemaphoreCreateMutex();
    s_ring = heap_caps_calloc(OTBUS_FRAME_RING, sizeof(otbus_frame_t), MALLOC_CAP_SPIRAM);
    otb_init(&s_core);
    s_cmd = s_core.cmd;
    s_flags = s_core.flags;
    nvs_handle_t h;
    if (nvs_open("otbus", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "flags", &v) == ESP_OK) {
            s_flags = (otb_flags_t){.status_ca = v & 1, .mirror_57 = (v >> 1) & 1, .explicit_off = (v >> 2) & 1};
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "flags: ca=%d mirror57=%d explicitoff=%d", s_flags.status_ca, s_flags.mirror_57, s_flags.explicit_off);
    ot_init();
    // Above the thermostat task (20), on core 1 with it; the UI and Wi-Fi run on core 0.
    xTaskCreatePinnedToCore(bus_task, "otbus", 4096, NULL, 21, NULL, 1);
    ESP_LOGI(TAG, "bus task started (%s boiler)", s_sim ? "simulated" : "real");
}

void otbus_set_cmd(const otb_cmd_t *cmd)
{
    LOCK();
    s_cmd = *cmd;
    s_cmd_at_us = esp_timer_get_time();
    UNLOCK();
}

void otbus_get(otb_status_t *st)
{
    LOCK();
    *st = s_pub;
    UNLOCK();
}

void otbus_get_mode(otb_status_t *st, bool *sim)
{
    LOCK();
    *st = s_pub;
    *sim = s_pub_sim;
    UNLOCK();
}

void otbus_get_support(uint8_t out[128])
{
    LOCK();
    memcpy(out, s_support, 128);
    UNLOCK();
}

void otbus_get_flags(otb_flags_t *f)
{
    LOCK();
    *f = s_flags;
    UNLOCK();
}

void otbus_set_flags(const otb_flags_t *f, bool save)
{
    LOCK();
    s_flags = *f;
    UNLOCK();
    if (!save) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open("otbus", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "flags", (uint8_t)(f->status_ca | f->mirror_57 << 1 | f->explicit_off << 2));
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "flags saved: ca=%d mirror57=%d explicitoff=%d", f->status_ca, f->mirror_57, f->explicit_off);
}

bool otbus_manual(uint8_t type, uint8_t id, uint16_t data)
{
    LOCK();
    bool ok = !s_manual_req.pending;
    if (ok) {
        s_manual_req = (otb_manual_t){.pending = true, .type = type, .id = id, .data = data};
        s_manual_pub.pending = true;
    }
    UNLOCK();
    return ok;
}

void otbus_manual_get(otb_manual_t *m)
{
    LOCK();
    *m = s_manual_pub;
    UNLOCK();
}

void otbus_set_silent(bool silent)
{
    LOCK();
    s_silent = silent;
    UNLOCK();
}

void otbus_set_simulated(bool sim)
{
    if (sim == s_sim) {
        return;
    }
    s_sim = sim;
    ESP_LOGI(TAG, "boiler: %s", sim ? "simulated" : "real");
}

bool otbus_simulated(void)
{
    return s_sim;
}

bool otbus_sim_room(float *room_c)
{
    if (!s_sim || !s_sim_inited) {
        return false;
    }
    xSemaphoreTake(s_sim_lock, portMAX_DELAY);
    *room_c = s_bsim.plant.room_c;
    xSemaphoreGive(s_sim_lock);
    return true;
}

/* -------------------------------------------------------------- commands */

static bool on_off(const char *v)
{
    return v && (atoi(v) != 0 || strcmp(v, "on") == 0);
}

void otbus_command(const char *cmd, char *out, size_t size)
{
    char buf[64];
    strlcpy(buf, cmd, sizeof(buf));
    char *save = NULL;
    char *k = strtok_r(buf, " ", &save);
    char *v = strtok_r(NULL, " ", &save);
    if (!k) {
        k = "";
    }
    // Flags (real and simulated boiler)
    if (strcmp(k, "ca") == 0 || strcmp(k, "mirror57") == 0 || strcmp(k, "explicitoff") == 0) {
        otb_flags_t f;
        otbus_get_flags(&f);
        bool *p = k[0] == 'c' ? &f.status_ca : (k[0] == 'm' ? &f.mirror_57 : &f.explicit_off);
        if (v) {
            *p = on_off(v);
            otbus_set_flags(&f, true);
        }
        snprintf(out, size, "flags (saved): ca=%d mirror57=%d explicitoff=%d", f.status_ca, f.mirror_57, f.explicit_off);
        return;
    }
    // Simulator
    xSemaphoreTake(s_sim_lock, portMAX_DELAY);
    if (!s_sim_inited) {
        sim_init_locked();
    }
    ot_sim_t *b = &s_bsim;
    if (strcmp(k, "out") == 0 && v) {
        b->plant_p.t_out = strtof(v, NULL);
    } else if (strcmp(k, "swing") == 0 && v) {
        b->plant_p.out_swing = strtof(v, NULL);
    } else if (strcmp(k, "room") == 0 && v) {
        b->plant.room_c = strtof(v, NULL);
    } else if (strcmp(k, "miss") == 0 && v) {
        b->q.miss_pct = (uint8_t)atoi(v);
    } else if (strcmp(k, "fault") == 0 && v) {
        ot_sim_set_fault(b, (uint8_t)atoi(v));
    } else if (strcmp(k, "pressure") == 0 && v) {
        b->plant.pressure_bar = strtof(v, NULL);
    } else if (strcmp(k, "leak") == 0 && v) {
        b->plant_p.leak_bar_per_day = strtof(v, NULL);
    } else if (strcmp(k, "needca") == 0 && v) {
        b->q.need_ca = on_off(v);
    } else if (strcmp(k, "heats57") == 0 && v) {
        b->q.heats_to_57 = on_off(v);
    } else if (strcmp(k, "ignoreoff") == 0 && v) {
        b->q.ignores_ch_off = on_off(v);
    } else if (strcmp(k, "id14") == 0 && v) {
        b->q.honours_id14 = on_off(v);
    } else if (strcmp(k, "dhwwrite") == 0 && v) {
        b->q.dhw_set_writable = on_off(v);
    } else if (strcmp(k, "silence") == 0 && v) {
        b->q.miss_pct = on_off(v) ? 100 : 0;  // master's frames go unanswered (models a dead line)
    } else if (strcmp(k, "powercycle") == 0) {
        ot_sim_power_cycle(b);
    } else if (strcmp(k, "reset") == 0) {
        sim_init_locked();
    } else if (k[0] && strcmp(k, "show") != 0) {
        xSemaphoreGive(s_sim_lock);
        snprintf(out, size,
                 "unknown. SIM show|reset|powercycle|out C|swing K|room C|miss %%|silence 0/1|fault code|pressure bar|"
                 "leak bar/day|needca|heats57|ignoreoff|dhwwrite|id14 0/1; ca|mirror57|explicitoff 0/1");
        return;
    }
    bool ch;
    float tgt = ot_sim_effective_target(b, &ch);
    snprintf(out, size,
             "sim: out %.1f room %.2f flow %.1f ret %.1f tank %.1f mod %.0f flame %d dhw %d target %.1f ch %d "
             "e31 %d locked %d fault %d miss %d%% starts %lu/%lu needca %d heats57 %d ignoreoff %d",
             b->plant.out_c, b->plant.room_c, b->plant.flow_c, b->plant.return_c, b->plant.tank_c,
             b->plant.modulation, b->plant.flame, b->plant.dhw_active, tgt, ch, b->e31, b->ch_locked, b->oem_fault,
             b->q.miss_pct, (unsigned long)b->plant.ch_starts, (unsigned long)b->plant.dhw_starts, b->q.need_ca,
             b->q.heats_to_57, b->q.ignores_ch_off);
    xSemaphoreGive(s_sim_lock);
}
