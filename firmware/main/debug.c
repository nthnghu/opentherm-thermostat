// Console commands. They arrive on the USB serial port and, behind the web login, through POST /api/cmd
// (webui.c), so everything here is reachable over the network by whoever has that login:
// "GO <screen>\n" switches screens; "SIM ..." drives the simulated boiler; "OTSTAT" prints the bus state; "CFG [name value]" lists/sets the control and schedule settings; "PROG [day text]" the weekly program; "BOOST|DHWBOOST|PASTEUR on|off", "AWAY <hours>|on|off"; "MODE heat|off", "SP <C>" for bench tests; "SNAP\n" makes the device answer "SNAP <bytes>\n" followed by the raw RGB565 frame buffer.
#include "esp_heap_caps.h"
#include "esp_core_dump.h"
#include "tlscert.h"
#include "driver/gpio.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "board.h"
#include "debug.h"
#include "esp_lvgl_port.h"
#include "ui.h"
#include "thermostat.h"
#include "motion.h"
#include "settings.h"
#include "ot_bus.h"
#include "ot_frame.h"
#include "schedule.h"
#include "alerts.h"
#include "cfgall.h"
#include "mqtt_ha.h"
#include "nvs.h"
#include "histstore.h"
#include <time.h>

// Runs one command line and writes its answer to o (the serial console, or a web request: /api/cmd).

#define TS_MAX 48
static struct {
    TaskHandle_t h;
    configRUN_TIME_COUNTER_TYPE rt;
} s_ts_prev[TS_MAX];
static configRUN_TIME_COUNTER_TYPE s_ts_prev_total;
static int s_ts_prev_n;

// Free memory in the pools the Wi-Fi link needs (the SDIO driver takes each 1536-byte frame from the DMA heap).
void debug_heap_stats(char *out, size_t size)
{
    snprintf(out, size, "dma free=%u largest=%u min=%u | internal free=%u largest=%u | psram free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

void debug_task_stats(char *out, size_t size)
{
#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY
    static TaskStatus_t st[TS_MAX];
    static float pct[TS_MAX];
    configRUN_TIME_COUNTER_TYPE total;
    int n = (int)uxTaskGetSystemState(st, TS_MAX, &total);
    configRUN_TIME_COUNTER_TYPE span = (total - s_ts_prev_total) * 2;  // two cores
    for (int i = 0; i < n; i++) {
        configRUN_TIME_COUNTER_TYPE before = 0;
        for (int j = 0; j < s_ts_prev_n; j++) {
            if (s_ts_prev[j].h == st[i].xHandle) {
                before = s_ts_prev[j].rt;
                break;
            }
        }
        pct[i] = span ? 100.0f * (float)(st[i].ulRunTimeCounter - before) / (float)span : 0;
    }
    for (int i = 0; i < n; i++) {
        s_ts_prev[i].h = st[i].xHandle;
        s_ts_prev[i].rt = st[i].ulRunTimeCounter;
    }
    s_ts_prev_n = n;
    s_ts_prev_total = total;
    size_t len = 0;
    out[0] = 0;
    for (int k = 0; k < n && len < size; k++) {  // busiest first: pick the maximum each time
        int b = -1;
        for (int i = 0; i < n; i++) {
            if (pct[i] >= 0 && (b < 0 || pct[i] > pct[b])) {
                b = i;
            }
        }
        if (b < 0 || (pct[b] < 0.5f && k >= 8)) {
            break;
        }
        int core = (int)xTaskGetCoreID(st[b].xHandle);
        len += (size_t)snprintf(out + len, size - len, "%s%s c%c %.1f%%", k ? ", " : "", st[b].pcTaskName,
                                core < 0 || core > 1 ? '*' : (char)('0' + core), pct[b]);
        pct[b] = -1;
    }
#else
    snprintf(out, size, "n/a (run-time stats off)");
#endif
}

void debug_exec(const char *line, FILE *o)
{
    if (strncmp(line, "HANG ", 5) == 0) {  // "HANG <s>": watchdog restart; "HANG <s> feed": command timeout only
        int n = atoi(line + 5);
        bool feed = strstr(line + 5, "feed") != NULL;
        thermo_test_hang(n, feed);
        fprintf(o, "HANG control task for %d s, %s\n", n < 1 ? 1 : (n > 60 ? 60 : n),
                feed ? "watchdog fed: expect heating off after 10 s" : "watchdog not fed: expect a restart after 10 s");
        return;
    }
    if (strcmp(line, "HEAP") == 0) {
        char b[200];
        debug_heap_stats(b, sizeof(b));
        fprintf(o, "HEAP %s\n", b);
        return;
    }
    if (strcmp(line, "PERF redraw") == 0 || strcmp(line, "PERF blank") == 0) {  // one full redraw of the screen shown
        if (lvgl_port_lock(1000)) {
            uint32_t ms = ui_perf_redraw(10, line[5] == 'b');
            lvgl_port_unlock();
            fprintf(o, "PERF %s: %lu ms per full-screen redraw\n", line + 5, (unsigned long)ms);
        }
        return;
    }
    if (strcmp(line, "PERF") == 0) {  // drawing speed since the last PERF
        uint32_t n, ms, mx;
        ui_perf(&n, &ms, &mx);
        fprintf(o, "PERF frames=%lu draw_ms=%lu avg_ms=%.1f max_ms=%lu\n", (unsigned long)n, (unsigned long)ms,
                n ? (double)ms / n : 0.0, (unsigned long)mx);
        return;
    }
    if (strcmp(line, "TASKS") == 0) {  // CPU per task since the last TASKS
        char *b = malloc(1536);
        if (b) {
            debug_task_stats(b, 1536);
            fprintf(o, "TASKS %s\n", b);
            free(b);
        }
        return;
    }
    if (strncmp(line, "GO ", 3) == 0) {
        lvgl_port_lock(0);
        lv_display_trigger_activity(NULL);  // or the idle timer jumps straight back to the thermostat
        ui_debug_go(line + 3);
        lvgl_port_unlock();
        return;
    }
    if (strcmp(line, "STAT") == 0) {
        thermo_state_t st;
        thermo_get(&st);
        char nl[12];
        sched_level_text(st.next_level, nl, sizeof(nl));
        fprintf(o, "STAT sensor_ok=%d raw=%.2f smooth=%.3f shown=%.1f hum=%.1f errors=%lu sp=%.1f demand=%d loops=%lu\n",
               st.sensor_ok, st.room_raw, st.room_c, st.room_shown, st.humidity, (unsigned long)st.sensor_errors, st.setpoint, st.demand, (unsigned long)st.loops);
        fprintf(o, "CTRL state=%s tset=%.1f req=%.1f p=%.2f i=%.2f duty=%.2f starts_h=%u flame=%d dhw=%d flow=%.1f ret=%.1f mod=%.0f\n",
               ctrl_state_name(st.ctrl_state), st.flow_setpoint, st.req_c, st.pi_p, st.pi_i, st.duty,
               st.starts_last_h, st.flame, st.dhw_active, st.flow_c, st.return_c, st.modulation);
        fprintf(o, "SCHED src=%s target=%.1f next=%d/%s boost=%d away=%d until=%lld summer=%d out=%d/%.1f mean=%d/%.1f "
               "dhw=%d/%s dhwboost=%d set=%.1f id56=%d tank=%d/%.1f pasteur=%d last=%s@%lld\n",
               sched_src_name(st.sched_src), st.target_c, st.next_change_min, nl,
               st.boost_left_s, st.away, (long long)st.away_until, st.summer, st.outside_valid, st.outside_c,
               st.outside_mean_valid, st.outside_mean_c, st.dhw_enable, sched_dhw_reason_name(st.dhw_reason),
               st.dhw_boost_left_s, st.dhw_set, st.id56, st.tank_valid, st.tank_c, st.pasteur_running,
               sched_pasteur_name(st.pasteur_result), (long long)st.pasteur_last);
        dl_kpi_t k1, k24;
        thermo_get_kpi(&k1, &k24);
        fprintf(o, "KPI 1h starts=%u | 24h(%.1fh) starts/h=%.2f burns=%u median=%.1fmin short=%.0f%% cond=%.0f%% "
               "mod=%.1f%% atmin=%.0f%% comfort=%.2fKh gas=%.2fkWh burner=%.2fh | total gas=%.3fkWh starts=%lu/%lu "
               "log=%d\n",
               k1.ch_starts, k24.hours, k24.ch_starts_h, k24.ch_burns, k24.median_burn_min, k24.short_pct,
               k24.cond_pct, k24.mean_mod, k24.min_mod_pct, k24.comfort_kh, k24.gas_kwh, k24.burner_h,
               st.gas_kwh_total, (unsigned long)st.ch_starts_total, (unsigned long)st.dhw_starts_total,
               thermo_log_count());
        char mst[96];
        mqtt_ha_status(mst, sizeof(mst));
        fprintf(o, "ALERT count=%u bits=%04x text=%s | MQTT %s\n", st.alert_count, st.alerts,
               st.alert_text[0] ? st.alert_text : "none", mst);
        char hl[600];
        board_wifi_hang_log(hl, sizeof(hl));
        fprintf(o, "WIFI hangs: %s\n", hl);
        char *dl = malloc(1600);
        if (dl) {
            board_wifi_drop_log(dl, 1600);
            fprintf(o, "WIFI drops (%lu): %s\n", (unsigned long)board_wifi_drops(), dl);
            free(dl);
        }
        histstore_status(hl, sizeof(hl));
        fprintf(o, "HIST %s | RAM %d minutes, %d hours\n", hl, thermo_log_count(), thermo_hour_count());
        fprintf(o, "WIFI up=%d rssi=%d chip_silent_s=%d wd_min=%u wd_restarts=%lu chip_resets=%lu\n", board_wifi_connected(),
               board_wifi_rssi(), board_wifi_cp_silent_s(), settings_get()->wifi_wd_min,
               (unsigned long)board_wifi_wd_restarts(), (unsigned long)board_wifi_chip_resets());
        motion_debug_t m;
        motion_debug(&m);
        fprintf(o, "MOT on=%d state=%s mean=%d contrast=%d changed=%d/576 delta=%d.%02d frames=%d grab_fail=%d "
               "detections=%lu idle_ms=%lu\n",
               settings_get()->motion_wake, motion_state_name(), m.mean, m.contrast, m.changed, m.delta_x100 / 100,
               m.delta_x100 % 100, m.frames, m.grab_fail,
               (unsigned long)motion_count(), (unsigned long)lv_display_get_inactive_time(NULL));
        return;
    }
    if (strncmp(line, "SIM", 3) == 0) {
        char out[400];
        if (strncmp(line, "SIM room ", 9) == 0 && (line[9] == '0' || line[9] == '1') && line[10] == 0) {
            thermo_set_sim_room(line[9] == '1');  // simulated house temperature replaces the SHT40
            fprintf(o, "SIM sim_room=%c\n", line[9]);
            return;
        }
        otbus_command(line[3] ? line + 4 : "", out, sizeof(out));
        fprintf(o, "SIM %s\n", out);
        return;
    }
    if (strcmp(line, "MODE heat") == 0 || strcmp(line, "MODE off") == 0) {
        thermo_set_mode(line[5] == 'h' ? THERMO_MODE_HEAT : THERMO_MODE_OFF);
        fprintf(o, "MODE %s\n", line + 5);
        return;
    }
    if (strcmp(line, "RESTART") == 0) {  // restarts 1 s later, after the answer went out
        static esp_timer_handle_t t;
        if (!t) {
            const esp_timer_create_args_t a = {.callback = (esp_timer_cb_t)esp_restart, .name = "restart"};
            esp_timer_create(&a, &t);
        }
        esp_timer_start_once(t, 1000 * 1000);
        fprintf(o, "RESTART in 1 s\n");
        return;
    }
    if (strncmp(line, "TARGET ", 7) == 0 || strcmp(line, "RESUME") == 0) {  // as the main-page dial / Program button
        if (line[0] == 'T') {
            thermo_set_target(strtof(line + 7, NULL));
        } else {
            thermo_resume_program();
        }
        vTaskDelay(pdMS_TO_TICKS(1200));  // one control step
        thermo_state_t st;
        thermo_get(&st);
        fprintf(o, "TARGET %.1f src=%s until=%lld\n", st.target_c, sched_src_name(st.sched_src),
                (long long)st.override_until);
        return;
    }
    if (strncmp(line, "SP ", 3) == 0) {
        thermo_set_setpoint(strtof(line + 3, NULL));
        thermo_state_t st;
        thermo_get(&st);
        fprintf(o, "SP %.1f\n", st.setpoint);
        return;
    }
    if (strncmp(line, "CFG", 3) == 0 && (line[3] == 0 || line[3] == ' ')) {
        // CFG = list, CFG <name> <value> = set (saved after 5 s), CFG defaults = factory values (program kept)
        char name[32] = "", value[24] = "";
        int n = sscanf(line + 3, "%31s %23s", name, value);
        if (n == 1 && strcmp(name, "defaults") == 0) {
            cfgall_defaults();
            fprintf(o, "CFG defaults restored (weekly program kept)\n");
        } else if (n == 2) {
            fprintf(o, "CFG %s = %s %s\n", name, value,
                   cfgall_set(name, value) ? "ok" : "refused (unknown name or out of range)");
        } else {
            char v[24];
            for (int i = 0; i < cfgall_count(); i++) {
                const ct_field_t *f = cfgall_field(i, NULL);
                cfgall_get(i, v, sizeof(v));
                fprintf(o, "CFG %-19s %-8s %s\n", f->name, v, f->help);
            }
        }
        return;
    }
    if (strncmp(line, "PROG", 4) == 0 && (line[4] == 0 || line[4] == ' ')) {
        // PROG = show the week; PROG <0..6> <HH:MM=c|e ...> = set one day (0 = Monday, empty = no points)
        static const char *days[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        sched_config_t sc;
        thermo_get_sched_config(&sc);
        int d = -1, off = 0;
        if (line[4] && sscanf(line + 5, "%d%n", &d, &off) == 1) {
            const char *text = line + 5 + off;
            while (*text == ' ') {
                text++;
            }
            if (sched_prog_parse(&sc, d, text)) {
                thermo_set_sched_config(&sc);
            } else {
                fprintf(o, "PROG refused (day 0..6, up to %d points HH:MM=c|e)\n", SCHED_POINTS);
            }
        }
        char buf[100];
        for (int i = 0; i < 7; i++) {
            sched_prog_format(&sc, i, buf, sizeof(buf));
            fprintf(o, "PROG %d %s %s\n", i, days[i], buf);
        }
        fprintf(o, "PROG prog_on=%d eco_setback_k=%.1f\n", sc.prog_on, sc.eco_setback_k);
        return;
    }
    if (strncmp(line, "SVC", 3) == 0 && (line[3] == 0 || line[3] == ' ')) {
        // SVC = status and record; SVC on|off; SVC run <name> [confirm]; SVC cancel; SVC apply <name>
        char a[16] = "", b[24] = "", c3[16] = "", why[128];
        int n = sscanf(line + 3, "%15s %23s %15s", a, b, c3);
        if (n >= 1 && (strcmp(a, "on") == 0 || strcmp(a, "off") == 0)) {
            thermo_svc_enter(a[1] == 'n');
        } else if (n >= 2 && strcmp(a, "run") == 0) {
            int r = svc_routine_by_name(b);
            bool ok = r >= 0 && thermo_svc_start((svc_routine_t)r, strcmp(c3, "confirm") == 0, why, sizeof(why));
            fprintf(o, "SVC run %s: %s\n", b, r < 0 ? "unknown routine" : (ok ? "started" : why));
        } else if (n >= 1 && strcmp(a, "cancel") == 0) {
            thermo_svc_cancel();
        } else if (n >= 2 && strcmp(a, "apply") == 0) {
            int r = svc_routine_by_name(b);
            if (r >= 0) {
                thermo_svc_apply((svc_routine_t)r, why, sizeof(why));
            }
            fprintf(o, "SVC apply %s: %s\n", b, r < 0 ? "unknown routine" : why);
        }
        thermo_state_t st;
        thermo_get(&st);
        fprintf(o, "SVC active=%d idle_left=%ds running=%d routine=%s elapsed=%ds step=%s\n", st.svc_active,
               st.svc_idle_left_s, st.svc_running, st.svc_running ? svc_routine_name(st.svc_routine) : "-",
               st.svc_elapsed_s, st.svc_step);
        svc_record_t *rec = malloc(sizeof(svc_record_t));
        if (rec) {
            char mt[96];
            thermo_svc_get(rec, mt, sizeof(mt));
            for (int i = 0; i < SVC_COUNT; i++) {
                const svc_result_t *r = &rec->r[i];
                fprintf(o, "SVC %-10s %-7s%s v=%.1f/%.1f/%.1f/%.1f %s\n", svc_routine_name(i), svc_rstate_name(r->state),
                       r->sim ? " (sim)" : "", r->v[0], r->v[1], r->v[2], r->v[3], r->text);
            }
            fprintf(o, "SVC manual: %s\n", mt[0] ? mt : "-");
            free(rec);
        }
        return;
    }
    if (strncmp(line, "OT read ", 8) == 0 || strncmp(line, "OT write ", 9) == 0) {
        // Service mode only. OT read <id>; OT write <id> <value> (°C/% as f8.8, or 0x1234 raw)
        bool w = line[3] == 'w';
        char vs[24] = "";
        int id = -1;
        sscanf(line + (w ? 9 : 8), "%d %23s", &id, vs);
        uint16_t data = 0;
        if (w) {
            data = strncmp(vs, "0x", 2) == 0 ? (uint16_t)strtoul(vs, NULL, 16) : ot_to_f88(strtof(vs, NULL));
        }
        char why[96];
        bool ok = id >= 0 && id < 128 && (!w || vs[0]) && thermo_svc_manual(w, (uint8_t)id, data, why, sizeof(why));
        fprintf(o, "OT %s %d: %s (answer: SVC)\n", w ? "write" : "read", id, ok ? "queued" : (id < 0 ? "bad id" : why));
        return;
    }
    if (strcmp(line, "MOTION off") == 0 || strcmp(line, "MOTION on") == 0) {  // test: pause (not saved)
        motion_set_off(line[8] == 'f');
        fprintf(o, "MOT off=%d (until restart)\n", line[8] == 'f');
        return;
    }
    if (strncmp(line, "COREDUMP", 8) == 0) {  // "COREDUMP": what is stored; "COREDUMP clear": erase it
        if (esp_core_dump_image_check() != ESP_OK) {
            fprintf(o, "COREDUMP none stored\n");
        } else if (strstr(line, "clear")) {
            fprintf(o, "COREDUMP clear: %s\n", esp_err_to_name(esp_core_dump_image_erase()));
        } else {
            esp_core_dump_summary_t *cs = malloc(sizeof(*cs));
            if (cs && esp_core_dump_get_summary(cs) == ESP_OK) {
                fprintf(o, "COREDUMP task=%s pc=0x%08lx cause=%lu ra=0x%08lx firmware=%.9s (download: /api/coredump)\n",
                        cs->exc_task, (unsigned long)cs->exc_pc, (unsigned long)cs->ex_info.mcause,
                        (unsigned long)cs->ex_info.ra, (const char *)cs->app_elf_sha256);
            } else {
                fprintf(o, "COREDUMP stored, summary not readable (download: /api/coredump)\n");
            }
            free(cs);
        }
        return;
    }
    if (strcmp(line, "TLS") == 0 || strcmp(line, "TLS new") == 0) {  // the web certificate's pin; "TLS new": replace it
        if (line[3]) {
            tlscert_forget();
            fprintf(o, "TLS certificate forgotten: a new one is made at the next restart (browsers and tools must trust it again)\n");
        } else {
            fprintf(o, "TLS pin %s\n", tlscert_pin());
        }
        return;
    }
    if (strcmp(line, "C6RESET") == 0) {  // test: what the Wi-Fi watchdog does first
        board_wifi_chip_reset();
        fprintf(o, "C6RESET requested (the connection drops for a few seconds)\n");
        return;
    }
    if (strncmp(line, "WIFIWD ", 7) == 0) {  // minutes, 0 = never
        static settings_t c;
        settings_copy(&c);
        c.wifi_wd_min = (uint16_t)atoi(line + 7);
        settings_save(&c);
        fprintf(o, "WIFI wd_min=%u\n", c.wifi_wd_min);
        return;
    }
    if (strcmp(line, "MQTT") == 0) {
        // MQTT settings (never the password) and NVS usage
        settings_t *c = malloc(sizeof(settings_t));
        if (c) {
            settings_copy(c);
            char mst[96];
            mqtt_ha_status(mst, sizeof(mst));
            fprintf(o, "MQTT on=%d uri=%s user=%s pass=%s loc=%s dev=%s state=%s\n", c->mqtt_on, c->mqtt_uri,
                   c->mqtt_user[0] ? "set" : "empty", c->mqtt_pass[0] ? "set" : "empty", c->mqtt_loc, c->mqtt_dev,
                   mst);
            free(c);
        }
        nvs_stats_t ns;
        if (nvs_get_stats(NULL, &ns) == ESP_OK) {
            fprintf(o, "MQTT nvs used=%u free=%u total=%u\n", (unsigned)ns.used_entries, (unsigned)ns.free_entries,
                   (unsigned)ns.total_entries);
        }
        return;
    }
    if (strcmp(line, "MQTT on") == 0 || strcmp(line, "MQTT off") == 0) {
        settings_t *c = malloc(sizeof(settings_t));
        if (c) {
            settings_copy(c);
            c->mqtt_on = line[6] == 'n';
            bool ok = settings_save(c) == ESP_OK;
            free(c);
            if (ok) {
                mqtt_ha_restart();
            }
            fprintf(o, "MQTT %s\n", ok ? "saved" : "save failed");
        }
        return;
    }
    if (strcmp(line, "MQTT dump") == 0) {
        mqtt_ha_dump();
        return;
    }
    if (strcmp(line, "BOOST on") == 0 || strcmp(line, "BOOST off") == 0) {
        fprintf(o, "BOOST %s\n", thermo_boost(line[7] == 'n') ? "ok" : "refused (boost_on 0)");
        return;
    }
    if (strcmp(line, "DHWBOOST on") == 0 || strcmp(line, "DHWBOOST off") == 0) {
        fprintf(o, "DHWBOOST %s\n", thermo_dhw_boost(line[10] == 'n') ? "ok" : "refused (dhw_boost_min 0 or dhw_mode 2)");
        return;
    }
    if (strcmp(line, "PASTEUR on") == 0 || strcmp(line, "PASTEUR off") == 0) {
        fprintf(o, "PASTEUR %s\n", thermo_pasteurise(line[9] == 'n') ? "ok" : "refused (pasteur_on 0 or dhw_mode 2)");
        return;
    }
    if (strncmp(line, "AWAY ", 5) == 0) {
        // AWAY on = until cancelled, AWAY <hours> = until then, AWAY off
        bool ok;
        if (strcmp(line + 5, "off") == 0) {
            ok = thermo_away(false, 0);
        } else if (strcmp(line + 5, "on") == 0) {
            ok = thermo_away(true, 0);
        } else {
            float hrs = strtof(line + 5, NULL);
            ok = hrs > 0 && thermo_away(true, (int64_t)time(NULL) + (int64_t)(hrs * 3600));
        }
        fprintf(o, "AWAY %s\n", ok ? "ok" : "refused (away_on 0 or bad hours)");
        return;
    }
    if (strcmp(line, "OTSTAT") == 0) {
        otb_status_t b;
        otbus_get(&b);
        fprintf(o, "OT %s link=%s flags=%02X flow=%.1f ret=%.1f mod=%.0f press=%.2f tank=%.1f tset=%.1f acked=%d "
               "fault=%02X/%u member=%u caps=%ukW/%u%% frames=%lu ok=%lu to=%lu bad=%lu unk=%lu retries=%lu "
               "losses=%lu lat=%u/%ums\n",
               otbus_simulated() ? "sim" : "real", otb_link_name(b.link), b.slave_flags, b.flow_c, b.return_c,
               b.modulation, b.pressure_bar, b.tank_c, b.tset_sent, b.tset_acked, b.fault_flags, b.oem_fault,
               b.member_id, b.max_cap_kw, b.min_mod_pct, (unsigned long)b.frames, (unsigned long)b.ok,
               (unsigned long)b.timeouts, (unsigned long)b.bad, (unsigned long)b.unknown_id,
               (unsigned long)b.retries, (unsigned long)b.link_losses, b.last_latency_ms, b.max_latency_ms);
        uint8_t sup[128];
        otbus_get_support(sup);
        fprintf(o, "OT supported:");
        for (int i = 0; i < 128; i++) {
            if (sup[i] == OTB_ID_SUPPORTED) {
                fprintf(o, " %d", i);
            }
        }
        fprintf(o, "\nOT unsupported:");
        for (int i = 0; i < 128; i++) {
            if (sup[i] == OTB_ID_UNSUPPORTED) {
                fprintf(o, " %d", i);
            }
        }
        fprintf(o, "\n");
        return;
    }
}

static void debug_task(void *arg)
{
    char line[128];
    size_t n = 0;
    for (;;) {
        char c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c != '\n' && n < sizeof(line) - 1) {
            line[n++] = c;
            continue;
        }
        line[n] = 0;
        n = 0;
        if (strcmp(line, "SNAP") != 0) {
            debug_exec(line, stdout);
            fflush(stdout);
            continue;
        }
        const uint8_t *fb = (const uint8_t *)board_framebuffer();
        if (!fb) {
            continue;
        }
        const size_t len = BOARD_LCD_H_RES * BOARD_LCD_V_RES * 2;
        esp_log_level_set("*", ESP_LOG_NONE);
        char hdr[32];
        int h = snprintf(hdr, sizeof(hdr), "\nSNAP %u\n", (unsigned)len);
        usb_serial_jtag_write_bytes(hdr, h, portMAX_DELAY);
        for (size_t off = 0; off < len; off += 4096) {
            size_t k = len - off < 4096 ? len - off : 4096;
            size_t done = 0;
            while (done < k) {
                done += usb_serial_jtag_write_bytes(fb + off + done, k - done, portMAX_DELAY);
            }
        }
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000));
        esp_log_level_set("*", ESP_LOG_INFO);
    }
}

void debug_start(void)
{
    usb_serial_jtag_driver_config_t cfg = {.rx_buffer_size = 256, .tx_buffer_size = 8192};
    if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) {
        usb_serial_jtag_vfs_use_driver();  // keep the console (logs) working through the driver
        xTaskCreatePinnedToCore(debug_task, "debug", 6144, NULL, 2, NULL, 0);
    }
}
