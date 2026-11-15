// Development helper on the USB serial port only (not reachable over the network):
// "GO <screen>\n" switches screens; "SIM ..." drives the simulated boiler; "OTSTAT" prints the bus state; "SNAP\n" makes the device answer "SNAP <bytes>\n" followed by the raw RGB565 frame buffer.
#include <string.h>
#include <stdio.h>
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

static void debug_task(void *arg)
{
    char line[80];
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
