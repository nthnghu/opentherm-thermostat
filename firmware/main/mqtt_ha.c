#include "mqtt_ha.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "mqtt_client.h"
#include "board.h"
#include "settings.h"
#include "thermostat.h"
#include "ot_bus.h"
#include "cfgall.h"

static const char *TAG = "mqtt";

#define STATE_PERIOD_US (10LL * 1000000)
#define RSSI_PERIOD_US  (10LL * 1000000)
#define JSON_MAX        4096

static esp_mqtt_client_handle_t s_client;
static SemaphoreHandle_t s_lock;
static volatile bool s_connected, s_need_discovery, s_need_state, s_restart;
static char s_status[96] = "starting";
static char s_base[80], s_node[48];
static char *s_json;  // PSRAM
static int s_rssi;   // cached: the Wi-Fi call goes over SDIO to the C6

/* ------------------------------------------------------------- helpers */

static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    vsnprintf(s_status, sizeof(s_status), fmt, ap);
    xSemaphoreGive(s_lock);
    va_end(ap);
}

void mqtt_ha_status(char *out, size_t size)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(out, size, "%s", s_status);
    xSemaphoreGive(s_lock);
}

typedef struct {
    char *buf;
    size_t n, size;
} jb_t;

static void jb_add(jb_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jb_add(jb_t *j, const char *fmt, ...)
{
    if (j->n >= j->size) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(j->buf + j->n, j->size - j->n, fmt, ap);
    va_end(ap);
    j->n = w < 0 ? j->size : j->n + (size_t)w;
}

static void jb_num(jb_t *j, const char *key, bool valid, const char *fmt, double v)
{
    jb_add(j, "%s\"%s\":", j->n > 1 ? "," : "", key);
    if (valid) {
        jb_add(j, fmt, v);
    } else {
        jb_add(j, "null");
    }
}

static void jb_str(jb_t *j, const char *key, const char *v)
{
    jb_add(j, "%s\"%s\":\"", j->n > 1 ? "," : "", key);
    for (const char *p = v; *p; p++) {
        if (*p == '"' || *p == '\\') {
            jb_add(j, "\\%c", *p);
        } else if ((unsigned char)*p >= 0x20) {
            jb_add(j, "%c", *p);
        }
    }
    jb_add(j, "\"");
}

// Topic-safe name: lowercase letters, digits, '_' and '-'.
static void clean_name(const char *in, char *out, size_t size, const char *fallback)
{
    size_t n = 0;
    for (const char *p = in; *p && n + 1 < size; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            out[n++] = c;
        }
    }
    out[n] = 0;
    if (!n) {
        snprintf(out, size, "%s", fallback);
    }
}

static void publish(const char *topic, const char *payload, int qos, bool retain)
{
    if (s_client && s_connected) {
        esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain);
    }
}

/* ----------------------------------------------------------- discovery */

typedef struct {
    const char *key, *name, *unit, *dev_cla, *stat_cla;
    int prec;      // suggested display precision, -1 = none
    bool diag;
} sensor_def_t;

static const sensor_def_t SENSORS[] = {
    {"room", "Room temperature", "°C", "temperature", "measurement", 1, false},
    {"humidity", "Humidity", "%", "humidity", "measurement", 0, false},
    {"target", "Target temperature", "°C", "temperature", "measurement", 1, false},
    {"flow_setpoint", "Flow setpoint", "°C", "temperature", "measurement", 1, false},
    {"requested", "Requested flow", "°C", "temperature", "measurement", 1, true},
    {"flow", "Flow temperature", "°C", "temperature", "measurement", 1, false},
    {"return", "Return temperature", "°C", "temperature", "measurement", 1, false},
    {"modulation", "Modulation", "%", NULL, "measurement", 0, false},
    {"pressure", "Water pressure", "bar", "pressure", "measurement", 2, false},
    {"tank", "Tank temperature", "°C", "temperature", "measurement", 1, false},
    {"outside", "Outdoor temperature (boiler)", "°C", "temperature", "measurement", 1, true},
    {"starts_1h", "Burner starts (last hour)", NULL, NULL, "measurement", 0, false},
    {"starts_h_24h", "Burner starts per hour (24 h)", NULL, NULL, "measurement", 2, false},
    {"median_burn_24h", "Median burn (24 h)", "min", "duration", "measurement", 1, false},
    {"short_pct_24h", "Short burns (24 h)", "%", NULL, "measurement", 0, false},
    {"cond_pct_24h", "Condensing burner time (24 h)", "%", NULL, "measurement", 0, false},
    {"mean_mod_24h", "Mean modulation (24 h)", "%", NULL, "measurement", 0, false},
    {"min_mod_pct_24h", "Time at minimum modulation (24 h)", "%", NULL, "measurement", 0, false},
    {"comfort_kh_24h", "Comfort deficit (24 h)", "K·h", NULL, "measurement", 2, false},
    {"burner_h_24h", "Burner hours (24 h)", "h", "duration", "measurement", 1, false},
    {"gas_kwh_24h", "Gas input estimate (24 h)", "kWh", NULL, "measurement", 1, false},
    {"gas_kwh_total", "Gas input estimate", "kWh", "energy", "total_increasing", 2, false},
    {"ch_starts_total", "Heating burner starts", NULL, NULL, "total_increasing", 0, true},
    {"dhw_starts_total", "Hot water burner starts", NULL, NULL, "total_increasing", 0, true},
    {"boost_left_min", "Boost time left", "min", "duration", NULL, 0, false},
    {"dhw_boost_left_min", "Hot water boost time left", "min", "duration", NULL, 0, false},
    {"duty", "Duty cycle", "%", NULL, "measurement", 0, true},
    {"pi_p", "PI proportional term", "°C", NULL, "measurement", 1, true},
    {"pi_i", "PI integral term", "°C", NULL, "measurement", 1, true},
    {"ot_errors", "OpenTherm errors", NULL, NULL, "total_increasing", 0, true},
    {"rssi", "Wi-Fi signal", "dBm", "signal_strength", "measurement", 0, true},
    {"uptime_s", "Uptime", "s", "duration", NULL, 0, true},
    // Text
    {"ctrl_state", "Controller state", NULL, NULL, NULL, -1, false},
    {"sched", "Schedule", NULL, NULL, NULL, -1, false},
    {"dhw_reason", "Hot water reason", NULL, NULL, NULL, -1, true},
    {"alert_text", "Alert", NULL, NULL, NULL, -1, false},
    {"pasteur_result", "Last pasteurisation", NULL, NULL, NULL, -1, true},
    {"service_routine", "Service routine", NULL, NULL, NULL, -1, true},
};

typedef struct {
    const char *key, *name, *dev_cla;
    bool diag;
} binary_def_t;

static const binary_def_t BINARIES[] = {
    {"flame", "Flame", "heat", false},
    {"dhw_active", "Hot water heating", "heat", false},
    {"demand", "Heat demand", "heat", false},
    {"dhw_enable", "Hot water enabled", NULL, true},
    {"summer", "Summer mode", NULL, false},
    {"boiler_online", "Boiler link", "connectivity", true},
    {"simulated", "Simulated boiler", NULL, true},
    {"service", "Service mode", NULL, true},
};

static void dev_block(jb_t *j)
{
    const esp_app_desc_t *app = esp_app_get_description();
    jb_add(j, "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Thermostat\",\"mf\":\"DIY\","
              "\"mdl\":\"OpenTherm wall thermostat (ESP32-P4)\",\"sw\":\"%s\"},"
              "\"~\":\"%s\",\"avty_t\":\"~/status/availability\"",
           s_node, app->version, s_base);
}

static void disc_publish(const char *component, const char *object, jb_t *j)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "homeassistant/%s/%s/%s/config", component, s_node, object);
    publish(topic, j->buf, 1, true);
}

static void send_discovery(void)
{
    jb_t j = {.buf = s_json, .size = JSON_MAX};

    // Climate entity: the temperature is the current target (what the screen shows and sets), mode, presets
    j.n = 0;
    jb_add(&j, "{");
    dev_block(&j);
    jb_add(&j, ",\"name\":null,\"uniq_id\":\"%s_climate\","
               "\"mode_cmd_t\":\"~/cmd/mode\",\"mode_stat_t\":\"~\",\"mode_stat_tpl\":\"{{ value_json.mode }}\","
               "\"modes\":[\"off\",\"heat\"],"
               "\"temp_cmd_t\":\"~/cmd/setpoint\",\"temp_stat_t\":\"~\",\"temp_stat_tpl\":\"{{ value_json.target }}\","
               "\"curr_temp_t\":\"~\",\"curr_temp_tpl\":\"{{ value_json.room }}\","
               "\"curr_hum_t\":\"~\",\"curr_hum_tpl\":\"{{ value_json.humidity }}\","
               "\"act_t\":\"~\",\"act_tpl\":\"{{ value_json.action }}\","
               "\"pr_mode_cmd_t\":\"~/cmd/preset\",\"pr_mode_stat_t\":\"~\","
               "\"pr_mode_val_tpl\":\"{{ value_json.preset }}\",\"pr_modes\":[\"boost\",\"away\"],"
               "\"min_temp\":%.0f,\"max_temp\":%.0f,\"temp_step\":0.5,\"precision\":0.1,\"temp_unit\":\"C\"}",
           s_node, THERMO_SP_MIN, THERMO_SP_MAX);
    disc_publish("climate", "climate", &j);

    for (size_t i = 0; i < sizeof(SENSORS) / sizeof(SENSORS[0]); i++) {
        const sensor_def_t *d = &SENSORS[i];
        j.n = 0;
        jb_add(&j, "{");
        dev_block(&j);
        jb_add(&j, ",\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"~\",\"val_tpl\":\"{{ value_json.%s }}\"",
               d->name, s_node, d->key, d->key);
        if (d->unit) {
            jb_add(&j, ",\"unit_of_meas\":\"%s\"", d->unit);
        }
        if (d->dev_cla) {
            jb_add(&j, ",\"dev_cla\":\"%s\"", d->dev_cla);
        }
        if (d->stat_cla) {
            jb_add(&j, ",\"stat_cla\":\"%s\"", d->stat_cla);
        }
        if (d->prec >= 0) {
            jb_add(&j, ",\"sug_dsp_prc\":%d", d->prec);
        }
        if (d->diag) {
            jb_add(&j, ",\"ent_cat\":\"diagnostic\"");
        }
        jb_add(&j, "}");
        disc_publish("sensor", d->key, &j);
    }

    for (size_t i = 0; i < sizeof(BINARIES) / sizeof(BINARIES[0]); i++) {
        const binary_def_t *d = &BINARIES[i];
        j.n = 0;
        jb_add(&j, "{");
        dev_block(&j);
        jb_add(&j, ",\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"~\",\"val_tpl\":\"{{ value_json.%s }}\","
                   "\"pl_on\":\"1\",\"pl_off\":\"0\"",
               d->name, s_node, d->key, d->key);
        if (d->dev_cla) {
            jb_add(&j, ",\"dev_cla\":\"%s\"", d->dev_cla);
        }
        if (d->diag) {
            jb_add(&j, ",\"ent_cat\":\"diagnostic\"");
        }
        jb_add(&j, "}");
        disc_publish("binary_sensor", d->key, &j);
    }

    // One problem sensor per alert
    for (int a = 0; a < ALERT_COUNT; a++) {
        j.n = 0;
        jb_add(&j, "{");
        dev_block(&j);
        jb_add(&j, ",\"name\":\"%s\",\"uniq_id\":\"%s_alert_%s\",\"stat_t\":\"~\","
                   "\"val_tpl\":\"{{ value_json.alert_%s }}\",\"pl_on\":\"1\",\"pl_off\":\"0\",\"dev_cla\":\"problem\"}",
               alert_title(a), s_node, alert_name(a), alert_name(a));
        char obj[40];
        snprintf(obj, sizeof(obj), "alert_%s", alert_name(a));
        disc_publish("binary_sensor", obj, &j);
    }

    // Everyday settings (the same values as the device Settings page)
    j.n = 0;
    jb_add(&j, "{");
    dev_block(&j);
    jb_add(&j, ",\"name\":\"Weekly program\",\"uniq_id\":\"%s_prog_on\",\"cmd_t\":\"~/cmd/prog_on\",\"stat_t\":\"~\","
               "\"val_tpl\":\"{{ value_json.prog_on }}\",\"pl_on\":\"1\",\"pl_off\":\"0\",\"icon\":\"mdi:calendar-clock\"}",
           s_node);
    disc_publish("switch", "prog_on", &j);
    j.n = 0;
    jb_add(&j, "{");
    dev_block(&j);
    jb_add(&j, ",\"name\":\"Summer mode (auto)\",\"uniq_id\":\"%s_summer_on\",\"cmd_t\":\"~/cmd/summer_on\","
               "\"stat_t\":\"~\",\"val_tpl\":\"{{ value_json.summer_on }}\",\"pl_on\":\"1\",\"pl_off\":\"0\","
               "\"icon\":\"mdi:weather-sunny\"}",
           s_node);
    disc_publish("switch", "summer_on", &j);
    j.n = 0;
    jb_add(&j, "{");
    dev_block(&j);
    jb_add(&j, ",\"name\":\"Hot water mode\",\"uniq_id\":\"%s_dhw_mode\",\"cmd_t\":\"~/cmd/dhw_mode\",\"stat_t\":\"~\","
               "\"val_tpl\":\"{{ value_json.dhw_mode }}\",\"options\":[\"Always\",\"Windows\",\"Off\"],"
               "\"icon\":\"mdi:water-boiler\"}",
           s_node);
    disc_publish("select", "dhw_mode", &j);
    j.n = 0;
    jb_add(&j, "{");
    dev_block(&j);
    jb_add(&j, ",\"name\":\"Eco setback\",\"uniq_id\":\"%s_eco_setback\",\"cmd_t\":\"~/cmd/eco_setback\",\"stat_t\":\"~\","
               "\"val_tpl\":\"{{ value_json.eco_setback }}\",\"min\":0.5,\"max\":6,\"step\":0.5,\"unit_of_meas\":\"K\","
               "\"mode\":\"box\",\"icon\":\"mdi:thermometer-minus\"}",
           s_node);
    disc_publish("number", "eco_setback", &j);

    // Hot water boost button
    j.n = 0;
    jb_add(&j, "{");
    dev_block(&j);
    jb_add(&j, ",\"name\":\"Hot water boost\",\"uniq_id\":\"%s_dhw_boost\",\"cmd_t\":\"~/cmd/dhw_boost\","
               "\"pl_prs\":\"on\",\"icon\":\"mdi:water-boiler\"}",
           s_node);
    disc_publish("button", "dhw_boost", &j);
    ESP_LOGI(TAG, "discovery sent (%s)", s_node);
}

/* --------------------------------------------------------------- state */

static bool build_state(void)
{
    thermo_state_t st;
    thermo_get(&st);
    dl_kpi_t k1, k24;
    thermo_get_kpi(&k1, &k24);
    jb_t j = {.buf = s_json, .size = JSON_MAX};
    jb_add(&j, "{");
    bool heat = st.mode == THERMO_MODE_HEAT;
    jb_num(&j, "room", st.sensor_ok, "%.2f", st.room_c);
    jb_num(&j, "humidity", st.sensor_ok, "%.1f", st.humidity);
    jb_num(&j, "sensor_ok", true, "%.0f", st.sensor_ok);
    jb_num(&j, "setpoint", true, "%.1f", st.setpoint);
    jb_num(&j, "target", true, "%.1f", st.target_c);
    jb_str(&j, "mode", heat ? "heat" : "off");
    jb_num(&j, "mode_heat", true, "%.0f", heat);
    jb_str(&j, "action", !heat && !st.demand ? "off" : (st.flame && !st.dhw_active && st.demand ? "heating" : "idle"));
    jb_str(&j, "preset", st.sched_src == SCHED_SRC_BOOST ? "boost" : (st.away ? "away" : "none"));
    jb_num(&j, "demand", true, "%.0f", st.demand);
    jb_num(&j, "flow_setpoint", true, "%.1f", st.flow_setpoint);
    jb_num(&j, "requested", true, "%.1f", st.req_c);
    jb_num(&j, "pi_p", true, "%.2f", st.pi_p);
    jb_num(&j, "pi_i", true, "%.2f", st.pi_i);
    jb_num(&j, "duty", true, "%.0f", st.duty * 100);
    jb_str(&j, "ctrl_state", ctrl_state_name(st.ctrl_state));
    jb_num(&j, "ctrl_state_id", true, "%.0f", st.ctrl_state);
    jb_str(&j, "sched", sched_src_name(st.sched_src));
    jb_num(&j, "sched_id", true, "%.0f", st.sched_src);
    jb_num(&j, "boost_left_min", true, "%.0f", (st.boost_left_s + 59) / 60);
    jb_num(&j, "away", true, "%.0f", st.away);
    jb_num(&j, "summer", true, "%.0f", st.summer);
    jb_num(&j, "boiler_online", true, "%.0f", st.boiler_online);
    jb_num(&j, "simulated", true, "%.0f", st.boiler_sim);
    jb_num(&j, "flame", true, "%.0f", st.flame);
    jb_num(&j, "dhw_active", true, "%.0f", st.dhw_active);
    jb_num(&j, "flow", st.boiler_online, "%.1f", st.flow_c);
    jb_num(&j, "return", st.boiler_online, "%.1f", st.return_c);
    jb_num(&j, "modulation", st.boiler_online, "%.0f", st.modulation > 100 ? 100 : st.modulation);
    jb_num(&j, "pressure", st.boiler_online && st.press_valid, "%.2f", st.pressure_bar);
    jb_num(&j, "tank", st.boiler_online && st.tank_valid, "%.1f", st.tank_c);
    jb_num(&j, "outside", st.outside_valid, "%.1f", st.outside_c);
    jb_num(&j, "dhw_enable", true, "%.0f", st.dhw_enable);
    jb_str(&j, "dhw_reason", sched_dhw_reason_name(st.dhw_reason));
    jb_num(&j, "dhw_boost_left_min", true, "%.0f", (st.dhw_boost_left_s + 59) / 60);
    jb_num(&j, "dhw_set", st.dhw_set > 0, "%.1f", st.dhw_set);
    jb_num(&j, "id56", true, "%.0f", st.id56);
    jb_str(&j, "pasteur_result", sched_pasteur_name(st.pasteur_result));
    jb_num(&j, "pasteur_running", true, "%.0f", st.pasteur_running);
    // KPIs
    jb_num(&j, "starts_1h", true, "%.0f", k1.ch_starts);
    jb_num(&j, "starts_h_24h", k24.hours > 0, "%.2f", k24.ch_starts_h);
    jb_num(&j, "median_burn_24h", k24.ch_burns > 0, "%.1f", k24.median_burn_min);
    jb_num(&j, "short_pct_24h", k24.ch_burns > 0, "%.1f", k24.short_pct);
    jb_num(&j, "cond_pct_24h", k24.burner_h > k24.dhw_burner_h, "%.1f", k24.cond_pct);
    jb_num(&j, "mean_mod_24h", k24.burner_h > k24.dhw_burner_h, "%.1f", k24.mean_mod);
    jb_num(&j, "min_mod_pct_24h", k24.burner_h > k24.dhw_burner_h, "%.1f", k24.min_mod_pct);
    jb_num(&j, "comfort_kh_24h", k24.hours > 0, "%.2f", k24.comfort_kh);
    jb_num(&j, "burner_h_24h", true, "%.2f", k24.burner_h);
    jb_num(&j, "gas_kwh_24h", true, "%.2f", k24.gas_kwh);
    jb_num(&j, "kpi_hours", true, "%.2f", k24.hours);
    jb_num(&j, "gas_kwh_total", true, "%.3f", st.gas_kwh_total);
    jb_num(&j, "ch_starts_total", true, "%.0f", st.ch_starts_total);
    jb_num(&j, "dhw_starts_total", true, "%.0f", st.dhw_starts_total);
    // Alerts
    jb_num(&j, "alerts", true, "%.0f", st.alert_count);
    jb_str(&j, "alert_text", st.alert_text[0] ? st.alert_text : "none");
    for (int a = 0; a < ALERT_COUNT; a++) {
        char key[40];
        snprintf(key, sizeof(key), "alert_%s", alert_name(a));
        jb_num(&j, key, true, "%.0f", (st.alerts >> a) & 1);
    }
    // Everyday settings
    sched_config_t sc;
    thermo_get_sched_config(&sc);
    static const char *dm[3] = {"Always", "Windows", "Off"};
    jb_num(&j, "prog_on", true, "%.0f", sc.prog_on);
    jb_num(&j, "summer_on", true, "%.0f", sc.summer_on);
    jb_str(&j, "dhw_mode", dm[sc.dhw_mode <= 2 ? sc.dhw_mode : 0]);
    jb_num(&j, "eco_setback", true, "%.1f", sc.eco_setback_k);
    // Service
    jb_num(&j, "service", true, "%.0f", st.svc_active);
    jb_str(&j, "service_routine", st.svc_running ? svc_routine_name(st.svc_routine) : "none");
    // Device
    jb_num(&j, "ot_errors", true, "%.0f", st.ot_errors);
    jb_num(&j, "rssi", s_rssi != 0, "%.0f", s_rssi);
    jb_num(&j, "uptime_s", true, "%.0f", esp_timer_get_time() / 1e6);
    jb_add(&j, "}");
    if (j.n >= j.size) {
        ESP_LOGE(TAG, "state JSON too long");
        return false;
    }
    return true;
}

static void send_state(void)
{
    if (build_state()) {
        publish(s_base, s_json, 0, false);
    }
}

void mqtt_ha_dump(void)
{
    if (!s_json) {
        return;
    }
    if (build_state()) {
        printf("MQTTSTATE %s\n", s_json);
    }
    // The climate discovery payload, as sent
    jb_t j = {.buf = s_json, .size = JSON_MAX};
    jb_add(&j, "{");
    if (!s_node[0]) {
        uint8_t mac[6] = {0};
        esp_efuse_mac_get_default(mac);
        snprintf(s_node, sizeof(s_node), "thermostat_%02x%02x%02x", mac[3], mac[4], mac[5]);
    }
    dev_block(&j);
    jb_add(&j, ",\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"~\",\"val_tpl\":\"{{ value_json.%s }}\"}",
           SENSORS[0].name, s_node, SENSORS[0].key, SENSORS[0].key);
    printf("MQTTDISC %s\n", s_json);
}

/* ------------------------------------------------------------ commands */

static void on_command(const char *name, const char *v)
{
    ESP_LOGI(TAG, "command %s = %.16s", name, v);  // the values are short words or numbers: never log more
    if (strcmp(name, "setpoint") == 0) {
        char *end;
        float c = strtof(v, &end);
        if (end != v && c >= THERMO_SP_MIN && c <= THERMO_SP_MAX) {
            thermo_set_target(c);  // like the screen: a hand override while the program runs, else the setpoint
        }
    } else if (strcmp(name, "mode") == 0) {
        if (strcmp(v, "heat") == 0) {
            thermo_set_mode(THERMO_MODE_HEAT);
        } else if (strcmp(v, "off") == 0) {
            thermo_set_mode(THERMO_MODE_OFF);
        }
    } else if (strcmp(name, "preset") == 0) {
        if (strcmp(v, "boost") == 0) {
            thermo_away(false, 0);
            thermo_boost(true);
        } else if (strcmp(v, "away") == 0) {
            thermo_boost(false);
            thermo_away(true, 0);
        } else if (strcmp(v, "none") == 0) {
            thermo_boost(false);
            thermo_away(false, 0);
        }
    } else if (strcmp(name, "prog_on") == 0 || strcmp(name, "summer_on") == 0) {
        cfgall_set(name, strcmp(v, "1") == 0 || strcasecmp(v, "on") == 0 ? "1" : "0");
    } else if (strcmp(name, "dhw_mode") == 0) {
        cfgall_set("dhw_mode", strcmp(v, "Always") == 0 ? "0" : (strcmp(v, "Windows") == 0 ? "1" : (strcmp(v, "Off") == 0 ? "2" : "x")));
    } else if (strcmp(name, "eco_setback") == 0) {
        cfgall_set("eco_setback_k", v);
    } else if (strcmp(name, "dhw_boost") == 0) {
        thermo_dhw_boost(strcmp(v, "off") != 0);
    }
    s_need_state = true;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    char topic[128];
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        snprintf(topic, sizeof(topic), "%s/status/availability", s_base);
        esp_mqtt_client_publish(s_client, topic, "online", 0, 1, true);
        snprintf(topic, sizeof(topic), "%s/cmd/+", s_base);
        esp_mqtt_client_subscribe(s_client, topic, 1);
        esp_mqtt_client_subscribe(s_client, "homeassistant/status", 1);
        s_need_discovery = true;
        s_need_state = true;
        set_status("connected (%s)", s_base);
        ESP_LOGI(TAG, "connected, base topic %s", s_base);
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected) {
            ESP_LOGW(TAG, "disconnected");
        }
        s_connected = false;
        set_status("disconnected, retrying");
        break;
    case MQTT_EVENT_ERROR:
        if (e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            int rc = e->error_handle->connect_return_code;
            set_status("refused by the broker (%s)", rc == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                                                             rc == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED
                                                         ? "user or password"
                                                         : "protocol");
        } else if (e->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            set_status("cannot reach the broker (network/firewall)");
        }
        break;
    case MQTT_EVENT_DATA: {
        if (e->total_data_len != e->data_len || e->current_data_offset != 0) {
            break;  // commands are short; ignore anything fragmented
        }
        char val[48];
        int n = e->data_len < (int)sizeof(val) - 1 ? e->data_len : (int)sizeof(val) - 1;
        memcpy(val, e->data, n);
        val[n] = 0;
        int tn = e->topic_len < (int)sizeof(topic) - 1 ? e->topic_len : (int)sizeof(topic) - 1;
        memcpy(topic, e->topic, tn);
        topic[tn] = 0;
        if (strcmp(topic, "homeassistant/status") == 0) {
            if (strcmp(val, "online") == 0) {
                s_need_discovery = true;  // HA restarted
                s_need_state = true;
            }
            break;
        }
        size_t bl = strlen(s_base);
        if (strncmp(topic, s_base, bl) == 0 && strncmp(topic + bl, "/cmd/", 5) == 0) {
            on_command(topic + bl + 5, val);
        }
        break;
    }
    default:
        break;
    }
}

/* ---------------------------------------------------------------- task */

static void stop_client(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
}

static void start_client(void)
{
    settings_t *c = malloc(sizeof(settings_t));
    if (!c) {
        return;
    }
    settings_copy(c);
    if (!c->mqtt_on || !c->mqtt_uri[0]) {
        set_status("disabled");
        free(c);
        return;
    }
    char loc[32], dev[32];
    clean_name(c->mqtt_loc, loc, sizeof(loc), "home");
    clean_name(c->mqtt_dev, dev, sizeof(dev), "thermostat");
    snprintf(s_base, sizeof(s_base), "sensors/%s/%s", loc, dev);
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(s_node, sizeof(s_node), "%s_%02x%02x%02x", dev, mac[3], mac[4], mac[5]);
    static char will[112];
    snprintf(will, sizeof(will), "%s/status/availability", s_base);
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = c->mqtt_uri,
        .credentials.username = c->mqtt_user[0] ? c->mqtt_user : NULL,
        .credentials.authentication.password = c->mqtt_pass[0] ? c->mqtt_pass : NULL,
        .credentials.client_id = s_node,
        .session.last_will = {.topic = will, .msg = "offline", .qos = 1, .retain = 1},
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 10000,
        .network.timeout_ms = 5000,
        .buffer.size = 2048,
        .buffer.out_size = 4096,
        .task.priority = 3,
        .task.stack_size = 6144,
    };
    s_client = esp_mqtt_client_init(&cfg);  // copies the strings
    free(c);
    if (!s_client) {
        set_status("error: bad broker address");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    set_status("connecting");
    esp_mqtt_client_start(s_client);
}

static void mqtt_task(void *arg)
{
    int64_t last_state = 0, last_rssi = 0;
    bool started = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        int64_t now = esp_timer_get_time();
        if (now - last_rssi >= RSSI_PERIOD_US) {
            last_rssi = now;
            s_rssi = board_wifi_connected() ? board_wifi_rssi() : 0;
            thermo_set_rssi(s_rssi);
        }
        if (s_restart || (!started && board_wifi_connected())) {
            s_restart = false;
            started = true;
            stop_client();
            start_client();
        }
        if (!s_connected) {
            continue;
        }
        if (s_need_discovery) {
            s_need_discovery = false;
            send_discovery();
        }
        if (s_need_state || now - last_state >= STATE_PERIOD_US) {
            s_need_state = false;
            last_state = now;
            send_state();
        }
    }
}

void mqtt_ha_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_json = heap_caps_malloc(JSON_MAX, MALLOC_CAP_SPIRAM);
    if (!s_json) {
        return;
    }
    xTaskCreatePinnedToCore(mqtt_task, "mqtt_ha", 6144, NULL, 3, NULL, 0);
}

void mqtt_ha_restart(void)
{
    s_restart = true;
}
