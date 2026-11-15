// Development helper on the USB serial port only (not reachable over the network):
// "GO <screen>\n" switches screens; "SIM ..." drives the simulated boiler; "OTSTAT" prints the bus state; "CFG [name value]" lists/sets the control and schedule settings; "PROG [day text]" the weekly program; "BOOST|DHWBOOST|PASTEUR on|off", "AWAY <hours>|on|off"; "MODE heat|off", "SP <C>" for bench tests; "SNAP\n" makes the device answer "SNAP <bytes>\n" followed by the raw RGB565 frame buffer.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
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
#include <time.h>

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
        if (strncmp(line, "GO ", 3) == 0) {
            lvgl_port_lock(0);
            lv_display_trigger_activity(NULL);  // or the idle timer jumps straight back to the thermostat
            ui_debug_go(line + 3);
            lvgl_port_unlock();
            continue;
        }
        if (strcmp(line, "STAT") == 0) {
            thermo_state_t st;
            thermo_get(&st);
            printf("STAT sensor_ok=%d raw=%.2f smooth=%.3f shown=%.1f hum=%.1f errors=%lu sp=%.1f demand=%d loops=%lu\n",
                   st.sensor_ok, st.room_raw, st.room_c, st.room_shown, st.humidity, (unsigned long)st.sensor_errors, st.setpoint, st.demand, (unsigned long)st.loops);
            printf("CTRL state=%s tset=%.1f req=%.1f p=%.2f i=%.2f duty=%.2f starts_h=%u flame=%d dhw=%d flow=%.1f ret=%.1f mod=%.0f\n",
                   ctrl_state_name(st.ctrl_state), st.flow_setpoint, st.req_c, st.pi_p, st.pi_i, st.duty,
                   st.starts_last_h, st.flame, st.dhw_active, st.flow_c, st.return_c, st.modulation);
            printf("SCHED src=%s target=%.1f next=%d/%s boost=%d away=%d until=%lld summer=%d out=%d/%.1f mean=%d/%.1f "
                   "dhw=%d/%s dhwboost=%d set=%.1f id56=%d tank=%d/%.1f pasteur=%d last=%s@%lld\n",
                   sched_src_name(st.sched_src), st.target_c, st.next_change_min, st.next_level ? "c" : "e",
                   st.boost_left_s, st.away, (long long)st.away_until, st.summer, st.outside_valid, st.outside_c,
                   st.outside_mean_valid, st.outside_mean_c, st.dhw_enable, sched_dhw_reason_name(st.dhw_reason),
                   st.dhw_boost_left_s, st.dhw_set, st.id56, st.tank_valid, st.tank_c, st.pasteur_running,
                   sched_pasteur_name(st.pasteur_result), (long long)st.pasteur_last);
            motion_debug_t m;
            motion_debug(&m);
            printf("MOT on=%d state=%s mean=%d contrast=%d changed=%d/576 delta=%d.%02d frames=%d grab_fail=%d "
                   "detections=%lu idle_ms=%lu\n",
                   settings_get()->motion_wake, motion_state_name(), m.mean, m.contrast, m.changed, m.delta_x100 / 100,
                   m.delta_x100 % 100, m.frames, m.grab_fail,
                   (unsigned long)motion_count(), (unsigned long)lv_display_get_inactive_time(NULL));
            continue;
        }
        if (strncmp(line, "SIM", 3) == 0) {
            char out[400];
            if (strncmp(line, "SIM room ", 9) == 0 && (line[9] == '0' || line[9] == '1') && line[10] == 0) {
                thermo_set_sim_room(line[9] == '1');  // simulated house temperature replaces the SHT40
                printf("SIM sim_room=%c\n", line[9]);
                continue;
            }
            otbus_command(line[3] ? line + 4 : "", out, sizeof(out));
            printf("SIM %s\n", out);
            continue;
        }
        if (strcmp(line, "MODE heat") == 0 || strcmp(line, "MODE off") == 0) {
            thermo_set_mode(line[5] == 'h' ? THERMO_MODE_HEAT : THERMO_MODE_OFF);
            printf("MODE %s\n", line + 5);
            continue;
        }
        if (strncmp(line, "SP ", 3) == 0) {
            thermo_set_setpoint(strtof(line + 3, NULL));
            thermo_state_t st;
            thermo_get(&st);
            printf("SP %.1f\n", st.setpoint);
            continue;
        }
        if (strncmp(line, "CFG", 3) == 0 && (line[3] == 0 || line[3] == ' ')) {
            // CFG = list, CFG <name> <value> = set (saved after 5 s), CFG defaults = factory values (both tables)
            ctrl_config_t c;
            sched_config_t sc;
            thermo_get_ctrl_config(&c);
            thermo_get_sched_config(&sc);
            char name[32] = "", value[24] = "";
            int n = sscanf(line + 3, "%31s %23s", name, value);
            if (n == 1 && strcmp(name, "defaults") == 0) {
                ctrl_default_config(&c);
                thermo_set_ctrl_config(&c);
                sched_default_config(&sc);
                thermo_set_sched_config(&sc);
                printf("CFG defaults restored\n");
            } else if (n == 2) {
                if (ctrl_config_set(&c, name, value)) {
                    thermo_set_ctrl_config(&c);
                    printf("CFG %s = %s ok\n", name, value);
                } else if (sched_config_set(&sc, name, value)) {
                    thermo_set_sched_config(&sc);
                    printf("CFG %s = %s ok\n", name, value);
                } else {
                    printf("CFG %s = %s refused (unknown name or out of range)\n", name, value);
                }
            } else {
                char v[24];
                for (int i = 0; i < ctrl_config_count(); i++) {
                    ctrl_config_get(&c, i, v, sizeof(v));
                    printf("CFG %-19s %-8s %s\n", ctrl_config_name(i), v, ctrl_config_help(i));
                }
                for (int i = 0; i < sched_config_count(); i++) {
                    sched_config_get(&sc, i, v, sizeof(v));
                    printf("CFG %-19s %-8s %s\n", sched_config_name(i), v, sched_config_help(i));
                }
            }
            continue;
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
                    printf("PROG refused (day 0..6, up to %d points HH:MM=c|e)\n", SCHED_POINTS);
                }
            }
            char buf[100];
            for (int i = 0; i < 7; i++) {
                sched_prog_format(&sc, i, buf, sizeof(buf));
                printf("PROG %d %s %s\n", i, days[i], buf);
            }
            printf("PROG prog_on=%d eco_setback_k=%.1f\n", sc.prog_on, sc.eco_setback_k);
            continue;
        }
        if (strcmp(line, "BOOST on") == 0 || strcmp(line, "BOOST off") == 0) {
            printf("BOOST %s\n", thermo_boost(line[7] == 'n') ? "ok" : "refused (boost_on 0)");
            continue;
        }
        if (strcmp(line, "DHWBOOST on") == 0 || strcmp(line, "DHWBOOST off") == 0) {
            printf("DHWBOOST %s\n", thermo_dhw_boost(line[10] == 'n') ? "ok" : "refused (dhw_boost_min 0 or dhw_mode 2)");
            continue;
        }
        if (strcmp(line, "PASTEUR on") == 0 || strcmp(line, "PASTEUR off") == 0) {
            printf("PASTEUR %s\n", thermo_pasteurise(line[9] == 'n') ? "ok" : "refused (pasteur_on 0 or dhw_mode 2)");
            continue;
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
            printf("AWAY %s\n", ok ? "ok" : "refused (away_on 0 or bad hours)");
            continue;
        }
        if (strcmp(line, "OTSTAT") == 0) {
            otb_status_t b;
            otbus_get(&b);
            printf("OT %s link=%s flags=%02X flow=%.1f ret=%.1f mod=%.0f press=%.2f tank=%.1f tset=%.1f acked=%d "
                   "fault=%02X/%u member=%u caps=%ukW/%u%% frames=%lu ok=%lu to=%lu bad=%lu unk=%lu retries=%lu "
                   "losses=%lu lat=%u/%ums\n",
                   otbus_simulated() ? "sim" : "real", otb_link_name(b.link), b.slave_flags, b.flow_c, b.return_c,
                   b.modulation, b.pressure_bar, b.tank_c, b.tset_sent, b.tset_acked, b.fault_flags, b.oem_fault,
                   b.member_id, b.max_cap_kw, b.min_mod_pct, (unsigned long)b.frames, (unsigned long)b.ok,
                   (unsigned long)b.timeouts, (unsigned long)b.bad, (unsigned long)b.unknown_id,
                   (unsigned long)b.retries, (unsigned long)b.link_losses, b.last_latency_ms, b.max_latency_ms);
            uint8_t sup[128];
            otbus_get_support(sup);
            printf("OT supported:");
            for (int i = 0; i < 128; i++) {
                if (sup[i] == OTB_ID_SUPPORTED) {
                    printf(" %d", i);
                }
            }
            printf("\nOT unsupported:");
            for (int i = 0; i < 128; i++) {
                if (sup[i] == OTB_ID_UNSUPPORTED) {
                    printf(" %d", i);
                }
            }
            printf("\n");
            continue;
        }
        if (strcmp(line, "SNAP") != 0) {
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
