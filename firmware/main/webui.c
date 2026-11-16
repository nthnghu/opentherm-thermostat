#include "webui.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <time.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "board.h"
#include "settings.h"
#include "thermostat.h"
#include "timekeep.h"
#include "selftest.h"
#include "ha.h"
#include "ot_bus.h"
#include "ot_frame.h"
#include "mqtt_ha.h"
#include "cfgall.h"
#include "cfgtext.h"
#include "cJSON.h"

static const char *TAG = "web";
static httpd_handle_t s_server;
static esp_timer_handle_t s_restart_timer, s_wifi_timer;

#define SEND(req, str) httpd_resp_send_chunk(req, str, HTTPD_RESP_USE_STRLEN)

/* ---------------------------------------------------------------- helpers */

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static void trim(char *s)
{
    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    memmove(s, start, strlen(start) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) {
        s[--n] = 0;
    }
}

static void send_escaped(httpd_req_t *req, const char *s)
{
    char buf[96];
    size_t n = 0;
    for (; *s; s++) {
        const char *rep = NULL;
        switch (*s) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        }
        size_t len = rep ? strlen(rep) : 1;
        if (n + len >= sizeof(buf)) {
            httpd_resp_send_chunk(req, buf, n);
            n = 0;
        }
        if (rep) {
            memcpy(buf + n, rep, len);
        } else {
            buf[n] = *s;
        }
        n += len;
    }
    if (n) {
        httpd_resp_send_chunk(req, buf, n);
    }
}

static void sendf(httpd_req_t *req, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sendf(httpd_req_t *req, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SEND(req, buf);
}

// Reads a form body (application/x-www-form-urlencoded). Caller frees.
static char *read_body(httpd_req_t *req, int max)
{
    int len = req->content_len;
    if (len <= 0 || len > max) {
        return NULL;
    }
    char *body = calloc(1, len + 1);
    int got = 0;
    while (body && got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) {
            free(body);
            return NULL;
        }
        got += r;
    }
    return body;
}

static bool form_get(const char *body, const char *key, char *out, size_t size)
{
    if (httpd_query_key_value(body, key, out, size) != ESP_OK) {
        return false;
    }
    url_decode(out);
    trim(out);
    return true;
}

/* ------------------------------------------------------------------ auth */

// Returns true when the request carries valid Basic credentials; otherwise sends 401/403.
static bool check_auth(httpd_req_t *req)
{
    if (!settings_web_configured()) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req, "<!doctype html><meta name=viewport content='width=device-width'>"
                                "<body style='font-family:system-ui;padding:20px'><h2>Web access is not set up</h2>"
                                "<p>Set a username and password on the thermostat first: "
                                "<b>Apps &rarr; Setup &rarr; Web access</b>.</p></body>");
        return false;
    }
    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        strncmp(hdr, "Basic ", 6) == 0) {
        unsigned char dec[100];
        size_t n = 0;
        if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &n, (const unsigned char *)hdr + 6, strlen(hdr + 6)) == 0) {
            dec[n] = 0;
            char *colon = strchr((char *)dec, ':');
            if (colon) {
                *colon = 0;
                if (settings_check_web_password((char *)dec, colon + 1)) {
                    return true;
                }
            }
        }
        ESP_LOGW(TAG, "bad login attempt");
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Thermostat\", charset=\"UTF-8\"");
    httpd_resp_sendstr(req, "Login required");
    return false;
}

/* ----------------------------------------------------------------- pages */

static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Thermostat</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:560px;margin:0 auto;padding:16px;background:#111a22;color:#e6edf3}"
    "h1{font-size:22px}h2{font-size:18px;margin-top:28px;border-top:1px solid #263238;padding-top:16px}"
    "label{display:block;margin:12px 0 4px;font-weight:600}"
    "input,textarea,select{width:100%;box-sizing:border-box;padding:9px;font-size:16px;border-radius:8px;"
    "border:1px solid #455a64;background:#1e2a38;color:#e6edf3}textarea{height:90px;font-family:monospace;font-size:13px}"
    "input[type=checkbox]{width:auto}button{margin-top:14px;padding:12px 18px;font-size:16px;border:0;border-radius:8px;"
    "background:#1565c0;color:#fff}.hint{color:#90a4ae;font-size:14px}.ok{color:#81c784}.err{color:#e57373}"
    ".warn{color:#ffb74d}a{color:#4fc3f7}table{border-collapse:collapse;width:100%}td{padding:4px 6px;border-bottom:1px solid #263238}"
    ".big{font-size:40px;font-weight:600}svg{width:100%;height:160px;background:#0d141b;border-radius:8px}"
    "</style></head><body>";

static void send_msg(httpd_req_t *req, const char *msg, bool ok)
{
    if (msg) {
        SEND(req, ok ? "<p class=ok>&#10004; " : "<p class=err>&#10008; ");
        send_escaped(req, msg);
        SEND(req, "</p>");
    }
}

static void input(httpd_req_t *req, const char *label, const char *name, const char *value, const char *type)
{
    sendf(req, "<label for=%s>%s</label><input id=%s name=%s type=%s autocapitalize=off autocorrect=off value=\"", name,
          label, name, name, type);
    send_escaped(req, value);
    SEND(req, "\">");
}

static esp_err_t send_page(httpd_req_t *req, const char *msg, bool ok)
{
    settings_t *c = malloc(sizeof(settings_t));
    if (!c) {
        return httpd_resp_send_500(req);
    }
    settings_copy(c);
    httpd_resp_set_type(req, "text/html");
    SEND(req, PAGE_HEAD);
    SEND(req, "<h1>Thermostat</h1>");
    send_msg(req, msg, ok);

    // Thermostat (read-only, refreshed by script)
    SEND(req, "<h2>Thermostat (view only)</h2><div class=big id=room>--</div>"
              "<div id=info class=hint>loading...</div><svg id=chart viewBox='0 0 1440 160' preserveAspectRatio=none></svg>"
              "<div class=hint>Last 24 h: room (blue), setpoint (orange), heating (red band).</div>");

    // Wi-Fi
    SEND(req, "<h2>Wi-Fi</h2><form method=post action=/save/wifi>");
    input(req, "Network name (SSID)", "ssid", c->ssid, "text");
    SEND(req, "<label for=pass>Password</label><input id=pass name=pass type=password placeholder='unchanged if empty'>"
              "<p class=hint>The device reconnects after saving; this page may need reloading at its new address.</p>"
              "<button>Save Wi-Fi</button></form>");
    sendf(req, "<form method=post action=/save/wifiwd><label for=wdmin>Restart when the Wi-Fi chip stops answering "
               "(minutes, 0 = never)</label><input id=wdmin name=min value='%u'><p class=hint>The Wi-Fi chip is a "
               "separate processor. If it hangs, the thermostat keeps heating but loses the network; only a restart "
               "brings it back (heating pauses for a few seconds). Now: last answer %d s ago, watchdog restarts so "
               "far: %lu.</p><button>Save</button></form>",
          c->wifi_wd_min, board_wifi_cp_silent_s(), (unsigned long)board_wifi_wd_restarts());

    // Home Assistant
    SEND(req, "<h2>Home Assistant</h2><form method=post action=/save/ha>");
    input(req, "HA URL", "url", c->ha_url, "text");
    sendf(req, "<label for=token>Long-lived access token</label><textarea id=token name=token spellcheck=false "
               "placeholder='%s'></textarea>",
          c->ha_token[0] ? "A token is saved. Paste a new one to replace it." : "Paste the token");
    SEND(req, "<p class=hint>HA: profile &rarr; Security &rarr; Long-lived access tokens &rarr; Create token. "
              "Or scan the token's QR code with the device camera (Setup &rarr; Home Assistant).</p>"
              "<p class=hint>Last test: ");
    send_escaped(req, ha_last_result()[0] ? ha_last_result() : "none yet");
    SEND(req, "</p><button>Save and test</button></form>");

    // MQTT
    SEND(req, "<h2>MQTT (Home Assistant, Grafana)</h2><form method=post action=/save/mqtt>");
    sendf(req, "<label><input type=checkbox name=on value=1%s> Connect</label>", c->mqtt_on ? " checked" : "");
    input(req, "Broker (mqtt://host:1883)", "uri", c->mqtt_uri, "text");
    input(req, "User", "user", c->mqtt_user, "text");
    sendf(req, "<label for=mpw>Password</label><input id=mpw name=pw type=password placeholder='%s'>",
          c->mqtt_pass[0] ? "unchanged if empty" : "");
    input(req, "Location (topic sensors/&lt;location&gt;/&lt;device&gt;)", "loc", c->mqtt_loc, "text");
    input(req, "Device", "dev", c->mqtt_dev, "text");
    char mst[96];
    mqtt_ha_status(mst, sizeof(mst));
    SEND(req, "<p class=hint>Home Assistant finds the thermostat by MQTT discovery. Numeric values also reach "
              "Telegraf/VictoriaMetrics as iot_&lt;field&gt;. State: ");
    send_escaped(req, mst);
    SEND(req, "</p><button>Save and connect</button></form>");

    // Service mode and commissioning (service.c)
    {
        thermo_state_t st;
        thermo_get(&st);
        svc_record_t *rec = malloc(sizeof(svc_record_t));
        char mt[96] = "";
        if (rec) {
            thermo_svc_get(rec, mt, sizeof(mt));
        }
        SEND(req, "<h2>Service (commissioning)</h2>");
        if (!st.svc_active) {
            SEND(req, "<p class=hint>Service mode runs the boiler test routines (docs/research ch. 10, \"Verify on the "
                      "real boiler\"). It ends by itself after 30 min without activity.</p>"
                      "<form method=post action=/save/svc><input type=hidden name=a value=on>"
                      "<button>Enter service mode</button></form>");
        } else {
            sendf(req, "<p>Service mode on (ends in %d min without activity). ", st.svc_idle_left_s / 60);
            if (st.svc_running) {
                sendf(req, "<b>Running: %s</b>, %d s: ", svc_routine_title(st.svc_routine), st.svc_elapsed_s);
                send_escaped(req, st.svc_step);
                SEND(req, "</p><form method=post action=/save/svc><input type=hidden name=a value=cancel>"
                          "<button>Cancel the routine</button></form>");
            } else {
                SEND(req, "</p>");
            }
            SEND(req, "<form method=post action=/save/svc><input type=hidden name=a value=off>"
                      "<button>Leave service mode</button></form>");
        }
        SEND(req, "<table><tr><th>Routine</th><th>Last result</th><th></th></tr>");
        for (int i = 0; rec && i < SVC_COUNT; i++) {
            const svc_result_t *r = &rec->r[i];
            sendf(req, "<tr><td><b>%s</b><br><span class=hint>", svc_routine_title(i));
            send_escaped(req, svc_routine_help(i));
            sendf(req, "</span></td><td>%s%s", svc_rstate_name(r->state), r->sim ? " (simulated boiler)" : "");
            if (r->epoch) {
                time_t t = (time_t)r->epoch;
                struct tm lt;
                localtime_r(&t, &lt);
                sendf(req, " %04d-%02d-%02d %02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour,
                      lt.tm_min);
            }
            SEND(req, "<br>");
            send_escaped(req, r->text);
            SEND(req, "</td><td>");
            if (st.svc_active && !st.svc_running) {
                sendf(req, "<form method=post action=/save/svc><input type=hidden name=a value=run>"
                           "<input type=hidden name=r value=%s>", svc_routine_name(i));
                if (i == SVC_COMM_LOSS) {
                    SEND(req, "<label><input type=checkbox name=confirm value=1> I can power-cycle the boiler</label>");
                }
                SEND(req, "<button>Start</button></form>");
            }
            svc_apply_t ap;
            if (rec && svc_recommend(rec, i, &ap)) {
                sendf(req, "<form method=post action=/save/svc><input type=hidden name=a value=apply>"
                           "<input type=hidden name=r value=%s><button>Apply</button></form>", svc_routine_name(i));
            }
            SEND(req, "</td></tr>");
        }
        SEND(req, "</table>");
        sendf(req, "<h3>Service PIN for the device</h3><p class=hint>%s</p><form method=post action=/save/svcpin>"
                   "<input name=pin type=password inputmode=numeric placeholder='4-6 digits, empty = remove'>"
                   "<button>Set PIN</button></form>",
              settings_svc_pin_configured() ? "A PIN is set. Settings > Service on the device asks for it."
                                            : "No PIN yet: the service menu on the device is locked.");
        if (st.svc_active && !st.svc_running) {
            SEND(req, "<h3>Read / write one Data-ID</h3><form method=post action=/save/svc>"
                      "<input type=hidden name=a value=manual><label for=mid>Data-ID</label>"
                      "<input id=mid name=id type=number min=0 max=127>"
                      "<label for=mval>Value to write (°C or %; 0x1234 = raw). Empty = read. Writes: IDs 1, 14, 56, 57 only"
                      "</label><input id=mval name=v type=text><button>Send</button></form><p class=hint>Last answer: ");
            send_escaped(req, mt[0] ? mt : "none");
            SEND(req, " (reload to see a new answer)</p>");
        }
        free(rec);
    }

    // All settings (cfgall: the same tables as the device editor and the serial CFG command)
    {
        SEND(req, "<h2>Thermostat settings</h2><p class=hint>The same settings as on the device "
                  "(Settings page). Indented settings belong to the one above them.</p>"
                  "<form method=post action=/save/cfg><input type=hidden name=cfgform value=1>");
        int last_g = -1;
        char val[24], txt[48], def[24], range[64];
        for (int n = 0; n < cfgtext_count(); n++) {
            const cfgtext_t *t = cfgtext_at(n);
            int i = cfgall_find(t->name);
            if (i < 0) {
                continue;
            }
            const ct_field_t *f = cfgall_field(i, NULL);
            if ((int)t->group != last_g) {
                sendf(req, "<h3>%s</h3><p class=hint>%s</p>", cfgtext_group_title(t->group),
                      cfgtext_group_brief(t->group));
                last_g = (int)t->group;
            }
            cfgall_get(i, val, sizeof(val));
            sendf(req, "<div title='%s'%s>", f->name, t->parent ? " style='margin-left:24px'" : "");
            if (f->type == CT_BOOL) {
                sendf(req, "<label><input type=checkbox name=%s value=1%s> %s</label>", f->name,
                      strcmp(val, "0") ? " checked" : "", t->title);
            } else if (t->opts) {
                sendf(req, "<label for=%s>%s</label><select id=%s name=%s>", f->name, t->title, f->name, f->name);
                for (int k = 0; k < t->nopts; k++) {
                    sendf(req, "<option value=%d%s>%s</option>", k, atoi(val) == k ? " selected" : "", t->opts[k].label);
                }
                SEND(req, "</select>");
            } else {
                const char *u = (f->type == CT_TIME || f->type == CT_TIME_OPT) ? "HH:MM" : t->unit;
                sendf(req, "<label for=%s>%s%s%s%s</label><input id=%s name=%s value='%s'>", f->name, t->title,
                      u[0] ? " (" : "", u, u[0] ? ")" : "", f->name, f->name, val);
            }
            SEND(req, "<p class=hint>");
            send_escaped(req, t->help);
            for (int k = 0; t->opts && k < t->nopts && t->opts[k].desc[0]; k++) {
                SEND(req, "<br><b>");
                send_escaped(req, t->opts[k].label);
                SEND(req, "</b>: ");
                send_escaped(req, t->opts[k].desc);
            }
            cfgtext_range(f, range, sizeof(range));
            cfgall_get_default(i, def, sizeof(def));
            cfgtext_format(f, def, txt, sizeof(txt));
            if (f->type == CT_TIME_OPT) {
                SEND(req, " Empty or - = not used.");
            }
            if (range[0]) {
                sendf(req, " Range %s.", range);
            }
            SEND(req, " Factory setting: ");
            send_escaped(req, txt);
            SEND(req, ".</p></div>");
        }
        SEND(req, "<button>Save settings</button></form>");

        sched_config_t sc;
        thermo_get_sched_config(&sc);
        SEND(req, "<h2>Weekly program</h2><form method=post action=/save/prog><p class=hint>Switch times per day, "
                  "e.g. <code>06:00=c 08:00=e 17:00=c 22:30=e</code> (c = Comfort = your setpoint, e = Eco). Empty = "
                  "the day continues the previous one. Used when Weekly program is ticked.</p>");
        static const char *dn[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
        char day[100];
        for (int d = 0; d < 7; d++) {
            sched_prog_format(&sc, d, day, sizeof(day));
            sendf(req, "<label for=prog%d>%s</label><input id=prog%d name=prog%d value='%s'>", d, dn[d], d, d, day);
        }
        SEND(req, "<button>Save program</button></form>");

        SEND(req, "<h2>Export / import</h2><p class=hint><a href='/api/config?download=1'>Download the configuration "
                  "(JSON)</a>: all settings, the weekly program and the boiler flags. Paste a saved file below to "
                  "restore it; unknown keys are ignored, values are range-checked.</p>"
                  "<form method=post action=/save/import><textarea name=json spellcheck=false "
                  "placeholder='{\"format\":\"thermostat-config-1\", ...}'></textarea><button>Import</button></form>");
    }

    // Clock
    SEND(req, "<h2>Clock</h2><form method=post action=/save/clock>");
    sendf(req, "<label><input type=checkbox name=ntp value=1 %s> Use NTP</label>", c->ntp_on ? "checked" : "");
    input(req, "NTP server", "server", c->ntp_server, "text");
    input(req, "Time zone (POSIX TZ)", "tz", c->tz, "text");
    {
        char now[40] = "not set";
        if (timekeep_valid()) {
            time_t t = time(NULL);
            struct tm lt;
            localtime_r(&t, &lt);
            strftime(now, sizeof(now), "%Y-%m-%d %H:%M:%S", &lt);
        }
        sendf(req, "<p class=hint>Device time: %s (%s). Europe/Budapest = CET-1CEST,M3.5.0,M10.5.0/3</p>", now,
              timekeep_source_name());
    }
    SEND(req, "<label for=manual>Set the time by hand (only used with NTP off)</label>"
              "<input id=manual name=manual type=datetime-local><button>Save clock</button></form>");

    // Display
    SEND(req, "<h2>Display</h2><form method=post action=/save/display>");
    sendf(req,
          "<label>Brightness %%</label><input name=bright type=number min=5 max=100 value=%u>"
          "<label>Dimmed brightness %%</label><input name=dim type=number min=1 max=100 value=%u>"
          "<label>Dim after (s, 0 = never)</label><input name=dim_s type=number min=0 max=3600 value=%u>",
          c->bright, c->dim_level, c->dim_after_s);
    sendf(req,
          "<label>Back to the thermostat after (s, 0 = never)</label><input name=home_s type=number min=0 max=3600 value=%u>"
          "<label><input type=checkbox name=night value=1 %s> Night mode</label>",
          c->home_after_s, c->night_on ? "checked" : "");
    sendf(req,
          "<label>Night from (HH:MM)</label><input name=n_st type=time value='%02u:%02u'>"
          "<label>Night until (HH:MM)</label><input name=n_en type=time value='%02u:%02u'>"
          "<label>Night brightness %%</label><input name=n_lvl type=number min=1 max=100 value=%u>",
          c->night_start / 60, c->night_start % 60, c->night_end / 60, c->night_end % 60, c->night_level);
    sendf(req,
          "<label>Screen off after (s, 0 = never)</label><input name=off_s type=number min=0 max=3600 value=%u>"
          "<label><input type=checkbox name=keep value=1 %s> Keep the screen on between</label>",
          c->off_after_s, c->keep_on ? "checked" : "");
    sendf(req,
          "<label>from (HH:MM)</label><input name=k_st type=time value='%02u:%02u'>"
          "<label>until (HH:MM)</label><input name=k_en type=time value='%02u:%02u'>",
          c->keep_on_start / 60, c->keep_on_start % 60, c->keep_on_end / 60, c->keep_on_end % 60);
    sendf(req,
          "<label><input type=checkbox name=motion value=1 %s> Wake on motion (camera)</label>"
          "<label>Motion sensitivity</label><select name=sens><option value=0 %s>Low</option>"
          "<option value=1 %s>Medium</option><option value=2 %s>High</option></select>",
          c->motion_wake ? "checked" : "", c->motion_sens == 0 ? "selected" : "", c->motion_sens == 1 ? "selected" : "",
          c->motion_sens == 2 ? "selected" : "");
    sendf(req,
          "<label>Theme</label><select name=theme><option value=0 %s>Auto (HA sun, else the hours below)</option>"
          "<option value=1 %s>Light</option><option value=2 %s>Dark</option></select>",
          c->theme_mode == 0 ? "selected" : "", c->theme_mode == 1 ? "selected" : "", c->theme_mode == 2 ? "selected" : "");
    sendf(req,
          "<label>Light theme from (HH:MM)</label><input name=d_st type=time value='%02u:%02u'>"
          "<label>until (HH:MM)</label><input name=d_en type=time value='%02u:%02u'>",
          c->day_start / 60, c->day_start % 60, c->day_end / 60, c->day_end % 60);
    SEND(req, "<button>Save display</button></form>");

    // Web login
    SEND(req, "<h2>Web login</h2><form method=post action=/save/web>");
    input(req, "Username", "user", c->web_user, "text");
    SEND(req, "<label>New password (min. 8 characters)</label><input name=pw type=password autocomplete=new-password>"
              "<button>Change login</button></form>");

    // Firmware
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    sendf(req, "<h2>Firmware</h2><p class=hint>Running %s %s from %s.</p>", app->project_name, app->version,
          run ? run->label : "?");
    SEND(req, "<input type=file id=fw accept=.bin><button onclick='upload()'>Upload and install</button>"
              "<p id=fwmsg class=hint>The device reboots into the new firmware and rolls back if its hardware check fails.</p>");

    // Diagnostics
    SEND(req, "<h2>Hardware check</h2><table>");
    const selftest_item_t *it;
    int n = selftest_items(&it);
    for (int i = 0; i < n; i++) {
        static const char *cls[] = {"hint", "ok", "warn", "err"};
        static const char *word[] = {"...", "OK", "warning", "FAILED"};
        sendf(req, "<tr><td>%s</td><td class=%s>%s</td><td class=hint>", it[i].name, cls[it[i].result],
              word[it[i].result]);
        send_escaped(req, it[i].detail);
        SEND(req, "</td></tr>");
    }
    sendf(req, "</table><p class=hint>IP %s, free heap %u KB internal / %u KB PSRAM, up %lld min.</p>", board_wifi_ip(),
          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), esp_timer_get_time() / 60000000);

    SEND(req,
         "<script>"
         "async function upd(){try{const r=await fetch('/api/status');const s=await r.json();"
         "document.getElementById('room').textContent=s.sensor_ok?s.room.toFixed(1)+' \\u00b0C':'sensor error';"
         "document.getElementById('info').textContent='Set '+s.setpoint.toFixed(1)+' \\u00b0C \\u00b7 '+s.mode+"
         "' \\u00b7 '+(s.demand?'heating':'idle')+' \\u00b7 humidity '+s.humidity.toFixed(0)+' % \\u00b7 boiler '+"
         "(s.boiler_sim?'simulated':'real')+(s.boiler_online?', flow '+s.flow.toFixed(1)+' \\u00b0C, mod '+s.mod.toFixed(0)+' %':', offline');"
         "const h=s.history,c=document.getElementById('chart');let lo=99,hi=-99;"
         "h.forEach(p=>{if(p[0]!=null){lo=Math.min(lo,p[0],p[1]);hi=Math.max(hi,p[0],p[1]);}});"
         "if(lo>hi){lo=15;hi=25;}lo-=0.5;hi+=0.5;const y=v=>160-(v-lo)/(hi-lo)*160,x=i=>i*1440/Math.max(h.length-1,1);"
         "let d='',sp='',band='';h.forEach((p,i)=>{if(p[0]!=null)d+=(d?'L':'M')+x(i)+' '+y(p[0]);"
         "sp+=(sp?'L':'M')+x(i)+' '+y(p[1]);if(p[2])band+=`<rect x='${x(i)}' y='150' width='${1440/h.length+1}' height='10' fill='#e57373'/>`;});"
         "c.innerHTML=band+`<path d='${sp}' stroke='#ffb74d' fill='none' stroke-width='3' vector-effect='non-scaling-stroke'/>`+"
         "`<path d='${d}' stroke='#4fc3f7' fill='none' stroke-width='3' vector-effect='non-scaling-stroke'/>`+"
         "`<text x='8' y='18' fill='#90a4ae' font-size='16'>${hi.toFixed(1)}</text><text x='8' y='154' fill='#90a4ae' font-size='16'>${lo.toFixed(1)}</text>`;"
         "}catch(e){document.getElementById('info').textContent='offline';}}"
         "upd();setInterval(upd,5000);"
         "async function upload(){const f=document.getElementById('fw').files[0],m=document.getElementById('fwmsg');"
         "if(!f){m.textContent='Choose a .bin file first';return;}m.textContent='Uploading '+f.name+'...';"
         "try{const r=await fetch('/ota',{method:'POST',body:f});m.textContent=await r.text();}catch(e){m.textContent='Upload failed: '+e;}}"
         "</script></body></html>");
    free(c);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t get_root(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    return send_page(req, NULL, false);
}

/* -------------------------------------------------------------- JSON API */

static esp_err_t get_status(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    thermo_state_t st;
    thermo_get(&st);
    httpd_resp_set_type(req, "application/json");
    sendf(req,
          "{\"sensor_ok\":%s,\"room\":%.2f,\"humidity\":%.1f,\"setpoint\":%.1f,\"mode\":\"%s\",\"demand\":%s,"
          "\"flow_setpoint\":%.1f,",
          st.sensor_ok ? "true" : "false", st.room_shown, st.humidity_shown, st.setpoint,
          st.mode == THERMO_MODE_HEAT ? "heat" : "off", st.demand ? "true" : "false", st.flow_setpoint);
    sendf(req,
          "\"boiler_sim\":%s,\"boiler_online\":%s,\"flame\":%s,\"flow\":%.1f,\"mod\":%.1f,\"offset\":%.1f,"
          "\"hysteresis\":%.2f,\"time_source\":\"%s\",",
          st.boiler_sim ? "true" : "false", st.boiler_online ? "true" : "false", st.flame ? "true" : "false", st.flow_c,
          st.modulation, st.offset, st.hysteresis, timekeep_source_name());
    sendf(req,
          "\"control\":{\"state\":\"%s\",\"requested\":%.1f,\"p\":%.2f,\"i\":%.2f,\"duty\":%.2f,"
          "\"starts_last_hour\":%u,\"return\":%.1f,\"dhw_active\":%s},",
          ctrl_state_name(st.ctrl_state), st.req_c, st.pi_p, st.pi_i, st.duty, st.starts_last_h, st.return_c,
          st.dhw_active ? "true" : "false");
    char mean[16] = "null";
    if (st.outside_mean_valid) {
        snprintf(mean, sizeof(mean), "%.1f", st.outside_mean_c);
    }
    sendf(req,
          "\"schedule\":{\"source\":\"%s\",\"target\":%.1f,\"next_change_min\":%d,\"next_level\":\"%s\","
          "\"boost_left_s\":%d,\"away\":%s,\"away_until\":%lld,\"summer\":%s,\"outside_mean\":%s},",
          sched_src_name(st.sched_src), st.target_c, st.next_change_min, st.next_level ? "comfort" : "eco",
          st.boost_left_s, st.away ? "true" : "false", (long long)st.away_until, st.summer ? "true" : "false",
          mean);
    sendf(req,
          "\"dhw\":{\"enable\":%s,\"reason\":\"%s\",\"boost_left_s\":%d,\"set_written\":%.1f,\"id56\":%u,"
          "\"tank\":%.1f,\"tank_valid\":%s,\"pasteur_running\":%s,\"pasteur_result\":\"%s\",\"pasteur_last\":%lld},",
          st.dhw_enable ? "true" : "false", sched_dhw_reason_name(st.dhw_reason), st.dhw_boost_left_s, st.dhw_set,
          st.id56, st.tank_c, st.tank_valid ? "true" : "false", st.pasteur_running ? "true" : "false",
          sched_pasteur_name(st.pasteur_result), (long long)st.pasteur_last);
    sendf(req, "\"alerts\":{\"count\":%u,\"text\":\"", st.alert_count);
    send_escaped(req, st.alert_text);
    SEND(req, "\",\"active\":[");
    bool firsta = true;
    for (int a = 0; a < ALERT_COUNT; a++) {
        if (st.alerts & (1u << a)) {
            sendf(req, "%s\"%s\"", firsta ? "" : ",", alert_name(a));
            firsta = false;
        }
    }
    SEND(req, "]},\"history\":[");
    // History, thinned to one point per 10 min: [room, setpoint, demand]
    thermo_sample_t *h = heap_caps_malloc(sizeof(thermo_sample_t) * THERMO_HISTORY_LEN, MALLOC_CAP_SPIRAM);
    int n = h ? thermo_history(h, THERMO_HISTORY_LEN) : 0;
    char buf[48];
    bool first = true;
    for (int i = n % 10; i < n; i += 10) {
        if (h[i].room_c10 == INT16_MIN) {
            snprintf(buf, sizeof(buf), "%s[null,%.1f,%d]", first ? "" : ",", h[i].setpoint_c10 / 10.0, h[i].demand);
        } else {
            snprintf(buf, sizeof(buf), "%s[%.1f,%.1f,%d]", first ? "" : ",", h[i].room_c10 / 10.0,
                     h[i].setpoint_c10 / 10.0, h[i].demand);
        }
        first = false;
        SEND(req, buf);
    }
    free(h);
    SEND(req, "]}");
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Control settings as JSON ({name: value, ...}); the same names as the serial CFG command.
static esp_err_t get_config(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    char q[32], v[24];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && strstr(q, "download")) {
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"thermostat-config.json\"");
    }
    httpd_resp_set_type(req, "application/json");
    const esp_app_desc_t *app = esp_app_get_description();
    sendf(req, "{\"format\":\"thermostat-config-1\",\"firmware\":\"%s\"", app->version);
    for (int i = 0; i < cfgall_count(); i++) {
        const ct_field_t *f = cfgall_field(i, NULL);
        cfgall_get(i, v, sizeof(v));
        sendf(req, ct_is_text(f, v) ? ",\"%s\":\"%s\"" : ",\"%s\":%s", f->name, v);
    }
    sched_config_t sc;
    thermo_get_sched_config(&sc);
    SEND(req, ",\"prog\":[");
    char day[100];
    for (int d = 0; d < 7; d++) {
        sched_prog_format(&sc, d, day, sizeof(day));
        sendf(req, "%s\"%s\"", d ? "," : "", day);
    }
    otb_flags_t fl;
    otbus_get_flags(&fl);
    sendf(req, "],\"bus_flags\":{\"ca\":%d,\"mirror57\":%d,\"explicitoff\":%d}}", fl.status_ca, fl.mirror_57,
          fl.explicit_off);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Applies an exported configuration (JSON text). Unknown keys are ignored; returns the count applied and
// lists refused names in msg.
static int import_config(const char *json, char *msg, size_t size)
{
    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(msg, size, "not a JSON object");
        return -1;
    }
    int applied = 0;
    size_t n = (size_t)snprintf(msg, size, "refused:");
    bool any_refused = false;
    for (cJSON *it = root->child; it; it = it->next) {
        if (!it->string || cfgall_find(it->string) < 0) {
            continue;
        }
        char v[24];
        if (cJSON_IsString(it)) {
            snprintf(v, sizeof(v), "%s", it->valuestring);
        } else if (cJSON_IsNumber(it)) {
            snprintf(v, sizeof(v), "%g", it->valuedouble);
        } else if (cJSON_IsBool(it)) {
            snprintf(v, sizeof(v), "%d", cJSON_IsTrue(it));
        } else {
            continue;
        }
        if (cfgall_set(it->string, v)) {
            applied++;
        } else if (n < size) {
            n += (size_t)snprintf(msg + n, size - n, " %s", it->string);
            any_refused = true;
        }
    }
    cJSON *prog = cJSON_GetObjectItem(root, "prog");
    if (cJSON_IsArray(prog) && cJSON_GetArraySize(prog) == 7) {
        sched_config_t sc;
        thermo_get_sched_config(&sc);
        bool ok = true;
        for (int d = 0; d < 7 && ok; d++) {
            cJSON *e = cJSON_GetArrayItem(prog, d);
            ok = cJSON_IsString(e) && sched_prog_parse(&sc, d, e->valuestring);
        }
        if (ok) {
            thermo_set_sched_config(&sc);
            applied++;
        } else if (n < size) {
            n += (size_t)snprintf(msg + n, size - n, " prog");
            any_refused = true;
        }
    }
    cJSON *fl = cJSON_GetObjectItem(root, "bus_flags");
    if (cJSON_IsObject(fl)) {
        otb_flags_t f;
        otbus_get_flags(&f);
        cJSON *x;
        if ((x = cJSON_GetObjectItem(fl, "ca")) && cJSON_IsNumber(x)) {
            f.status_ca = x->valueint != 0;
        }
        if ((x = cJSON_GetObjectItem(fl, "mirror57")) && cJSON_IsNumber(x)) {
            f.mirror_57 = x->valueint != 0;
        }
        if ((x = cJSON_GetObjectItem(fl, "explicitoff")) && cJSON_IsNumber(x)) {
            f.explicit_off = x->valueint != 0;
        }
        otbus_set_flags(&f, true);
        applied++;
    }
    cJSON_Delete(root);
    if (!any_refused) {
        msg[0] = 0;
    }
    return applied;
}

// OpenTherm bus state and the support map.
static esp_err_t get_ot(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    otb_status_t b;
    otbus_get(&b);
    otb_flags_t f;
    otbus_get_flags(&f);
    httpd_resp_set_type(req, "application/json");
    sendf(req,
          "{\"boiler\":\"%s\",\"link\":\"%s\",\"slave_flags\":%u,\"flow\":%.2f,\"return\":%.2f,\"modulation\":%.1f,"
          "\"pressure\":%.2f,\"tank\":%.2f,\"tset_sent\":%.1f,\"tset_acked\":%s,",
          otbus_simulated() ? "simulated" : "real", otb_link_name(b.link), b.slave_flags, b.flow_c, b.return_c,
          b.modulation, b.pressure_bar, b.tank_c, b.tset_sent, b.tset_acked ? "true" : "false");
    sendf(req,
          "\"fault_flags\":%u,\"oem_fault\":%u,\"member_id\":%u,\"slave_config\":%u,\"max_cap_kw\":%u,"
          "\"min_mod_pct\":%u,\"ch_bounds\":[%d,%d],\"dhw_bounds\":[%d,%d],\"dhw_set\":%.1f,\"maxset\":%.1f,",
          b.fault_flags, b.oem_fault, b.member_id, b.slave_config, b.max_cap_kw, b.min_mod_pct, b.ch_min, b.ch_max,
          b.dhw_min, b.dhw_max, b.dhw_set_c, b.maxset_c);
    sendf(req,
          "\"counters\":[%u,%u,%u,%u,%u,%u,%u,%u],\"valid\":%lu,\"flags\":{\"ca\":%s,\"mirror57\":%s,"
          "\"explicit_off\":%s},",
          b.counters[0], b.counters[1], b.counters[2], b.counters[3], b.counters[4], b.counters[5], b.counters[6],
          b.counters[7], (unsigned long)b.valid, f.status_ca ? "true" : "false", f.mirror_57 ? "true" : "false",
          f.explicit_off ? "true" : "false");
    sendf(req,
          "\"stats\":{\"cycles\":%lu,\"frames\":%lu,\"ok\":%lu,\"timeouts\":%lu,\"bad\":%lu,\"unknown_id\":%lu,"
          "\"data_invalid\":%lu,\"retries\":%lu,\"link_losses\":%lu,\"latency_last\":%u,\"latency_max\":%u,"
          "\"latency_avg\":%.1f},",
          (unsigned long)b.cycles, (unsigned long)b.frames, (unsigned long)b.ok, (unsigned long)b.timeouts,
          (unsigned long)b.bad, (unsigned long)b.unknown_id, (unsigned long)b.data_invalid,
          (unsigned long)b.retries, (unsigned long)b.link_losses, b.last_latency_ms, b.max_latency_ms,
          b.latency_n ? (double)b.latency_sum_ms / b.latency_n : 0.0);
    uint8_t sup[128];
    otbus_get_support(sup);
    SEND(req, "\"supported\":[");
    char buf[8];
    bool first = true;
    for (int i = 0; i < 128; i++) {
        if (sup[i] == OTB_ID_SUPPORTED) {
            snprintf(buf, sizeof(buf), "%s%d", first ? "" : ",", i);
            SEND(req, buf);
            first = false;
        }
    }
    SEND(req, "],\"unsupported\":[");
    first = true;
    for (int i = 0; i < 128; i++) {
        if (sup[i] == OTB_ID_UNSUPPORTED) {
            snprintf(buf, sizeof(buf), "%s%d", first ? "" : ",", i);
            SEND(req, buf);
            first = false;
        }
    }
    SEND(req, "]}");
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Frame capture as CSV (oldest first). ?n=<count> limits it to the last n frames.
static esp_err_t get_otlog(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    int max = OTBUS_FRAME_RING;
    char q[32], v[12];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "n", v, sizeof(v)) == ESP_OK) {
        int n = atoi(v);
        if (n > 0 && n < max) {
            max = n;
        }
    }
    otbus_frame_t *f = heap_caps_malloc(sizeof(otbus_frame_t) * max, MALLOC_CAP_SPIRAM);
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    int n = otbus_frames(f, max);
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"otlog.csv\"");
    // Wall-clock time of a frame = now - (uptime_now - t_ms), when the clock is set.
    int64_t up_ms = esp_timer_get_time() / 1000;
    time_t now = timekeep_valid() ? time(NULL) : 0;
    SEND(req, "uptime_ms,unix_time,source,result,latency_ms,request,response,id,req_type,resp_type,resp_value_f88\n");
    static const char *const res_names[] = {"ok", "timeout", "bad", "skip"};
    char *chunk = malloc(4096);
    int used = 0;
    for (int i = 0; chunk && i < n; i++) {
        const otbus_frame_t *e = &f[i];
        long long unix_t = now ? (long long)now - (up_ms - (int64_t)e->t_ms) / 1000 : 0;
        used += snprintf(chunk + used, 4096 - used, "%lu,%lld,%s,%s,%u,%08lX,%08lX,%u,%s,%s,%.2f\n",
                         (unsigned long)e->t_ms, unix_t, e->sim ? "sim" : "real", res_names[e->result & 3],
                         e->latency_ms, (unsigned long)e->req, (unsigned long)e->resp, ot_id(e->req),
                         ot_type_name(ot_type(e->req)), e->result == 0 ? ot_type_name(ot_type(e->resp)) : "",
                         e->result == 0 ? ot_f88(ot_data(e->resp)) : 0.0f);
        if (used > 4096 - 160) {
            httpd_resp_send_chunk(req, chunk, used);
            used = 0;
        }
    }
    if (chunk && used) {
        httpd_resp_send_chunk(req, chunk, used);
    }
    free(chunk);
    free(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ------------------------------------------------------------------ saves */

static void restart_cb(void *arg)
{
    esp_restart();
}

static char s_new_ssid[33], s_new_pass[65];

static void wifi_cb(void *arg)
{
    board_wifi_connect(s_new_ssid, s_new_pass);
}

static void kpi_json(httpd_req_t *req, const char *name, const dl_kpi_t *k, bool last)
{
    sendf(req,
          "\"%s\":{\"hours\":%.2f,\"ch_starts\":%u,\"dhw_starts\":%u,\"ch_starts_h\":%.2f,\"ch_burns\":%u,"
          "\"median_burn_min\":%.1f,\"short_pct\":%.1f,\"burner_h\":%.2f,\"dhw_burner_h\":%.2f,\"cond_pct\":%.1f,"
          "\"mean_mod\":%.1f,\"min_mod_pct\":%.1f,\"comfort_kh\":%.2f,\"over_kh\":%.2f,\"gas_kwh\":%.2f,"
          "\"mean_room\":%.2f,\"mean_target\":%.2f,\"mean_flow\":%.1f,\"mean_return\":%.1f,\"ot_errors\":%.0f}%s",
          name, k->hours, k->ch_starts, k->dhw_starts, k->ch_starts_h, k->ch_burns, k->median_burn_min, k->short_pct,
          k->burner_h, k->dhw_burner_h, k->cond_pct, k->mean_mod, k->min_mod_pct, k->comfort_kh, k->over_kh,
          k->gas_kwh, k->mean_room, k->mean_target, k->mean_flow, k->mean_return, k->ot_err, last ? "" : ",");
}

// Service mode state and the commissioning record.
static esp_err_t get_service(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    thermo_state_t st;
    thermo_get(&st);
    svc_record_t *rec = malloc(sizeof(svc_record_t));
    if (!rec) {
        return httpd_resp_send_500(req);
    }
    char mt[96];
    thermo_svc_get(rec, mt, sizeof(mt));
    httpd_resp_set_type(req, "application/json");
    sendf(req, "{\"active\":%s,\"running\":%s,\"routine\":\"%s\",\"elapsed_s\":%d,\"idle_left_s\":%d,\"step\":\"",
          st.svc_active ? "true" : "false", st.svc_running ? "true" : "false",
          st.svc_running ? svc_routine_name(st.svc_routine) : "", st.svc_elapsed_s, st.svc_idle_left_s);
    send_escaped(req, st.svc_step);
    SEND(req, "\",\"manual\":\"");
    send_escaped(req, mt);
    SEND(req, "\",\"results\":{");
    for (int i = 0; i < SVC_COUNT; i++) {
        const svc_result_t *r = &rec->r[i];
        sendf(req, "%s\"%s\":{\"state\":\"%s\",\"sim\":%s,\"time\":%lld,\"v\":[%.2f,%.2f,%.2f,%.2f],\"text\":\"", i ? "," : "",
              svc_routine_name(i), svc_rstate_name(r->state), r->sim ? "true" : "false", (long long)r->epoch, r->v[0], r->v[1], r->v[2], r->v[3]);
        send_escaped(req, r->text);
        SEND(req, "\"}");
    }
    SEND(req, "}}");
    free(rec);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// KPIs over the last hour and 24 h, plus totals since start.
static esp_err_t get_kpi(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    dl_kpi_t k1, k24;
    thermo_get_kpi(&k1, &k24);
    thermo_state_t st;
    thermo_get(&st);
    httpd_resp_set_type(req, "application/json");
    SEND(req, "{");
    kpi_json(req, "last_hour", &k1, false);
    kpi_json(req, "last_24h", &k24, false);
    sendf(req, "\"since_start\":{\"gas_kwh\":%.3f,\"ch_starts\":%lu,\"dhw_starts\":%lu,\"burner_h\":%.2f,"
               "\"log_minutes\":%d}}",
          st.gas_kwh_total, (unsigned long)st.ch_starts_total, (unsigned long)st.dhw_starts_total, st.burner_h_total,
          thermo_log_count());
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Minute log as CSV (?h=N limits to the last N hours; default all, up to 7 days).
static esp_err_t get_log(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    int n = thermo_log_count(), from = 0;
    char q[32], v[12];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "h", v, sizeof(v)) == ESP_OK) {
        int h = atoi(v);
        if (h > 0 && h * 60 < n) {
            from = n - h * 60;
        }
    }
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"thermostat-log.csv\"");
    SEND(req, datalog_csv_header());
    char *chunk = malloc(4096);
    int used = 0;
    dl_rec_t r;
    for (int i = from; chunk && i < n && thermo_log_get(i, &r); i++) {
        int w = datalog_csv_row(&r, chunk + used, 4096 - used);
        if (w < 0) {
            break;
        }
        used += w;
        if (used > 4096 - 400) {
            if (httpd_resp_send_chunk(req, chunk, used) != ESP_OK) {
                free(chunk);
                return ESP_FAIL;
            }
            used = 0;
        }
    }
    if (chunk && used) {
        httpd_resp_send_chunk(req, chunk, used);
    }
    free(chunk);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t post_save(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    char *body = read_body(req, 12288);  // the settings and import forms are a few KB
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
    }
    settings_t *c = malloc(sizeof(settings_t));
    char *v = malloc(512);
    if (!c || !v) {
        free(body);
        free(c);
        free(v);
        return httpd_resp_send_500(req);
    }
    settings_copy(c);
    const char *section = req->uri + strlen("/save/");
    const char *msg = "Saved";
    bool ok = true;
    char tmsg[128];

    if (strcmp(section, "wifi") == 0) {
        if (form_get(body, "ssid", v, 512) && v[0]) {
            strlcpy(s_new_ssid, v, sizeof(s_new_ssid));
            strlcpy(s_new_pass, c->pass, sizeof(s_new_pass));
            if (form_get(body, "pass", v, 512) && v[0]) {
                strlcpy(s_new_pass, v, sizeof(s_new_pass));
            }
            esp_timer_start_once(s_wifi_timer, 1500 * 1000);  // answer first, then reconnect
            msg = "Wi-Fi saved, reconnecting";
        } else {
            msg = "SSID missing";
            ok = false;
        }
    } else if (strcmp(section, "wifiwd") == 0) {
        int m = form_get(body, "min", v, 512) ? atoi(v) : -1;
        if (m < 0 || m > 240) {
            msg = "Enter 0 to 240 minutes";
            ok = false;
        } else {
            c->wifi_wd_min = (uint16_t)m;
            if (settings_save(c) != ESP_OK) {
                msg = "Could not save";
                ok = false;
            }
        }
    } else if (strcmp(section, "ha") == 0) {
        if (form_get(body, "url", v, 512)) {
            size_t n = strlen(v);
            if (n && v[n - 1] == '/') {
                v[n - 1] = 0;
            }
            if (v[0] && strncmp(v, "http://", 7) != 0 && strncmp(v, "https://", 8) != 0) {
                snprintf(c->ha_url, sizeof(c->ha_url), "http://%s", v);
            } else {
                strlcpy(c->ha_url, v, sizeof(c->ha_url));
            }
        }
        if (form_get(body, "token", v, 512) && v[0]) {
            strlcpy(c->ha_token, v, sizeof(c->ha_token));
        }
        settings_save(c);
        ok = ha_test(tmsg, sizeof(tmsg));
        msg = tmsg;
    } else if (strcmp(section, "mqtt") == 0) {
        c->mqtt_on = form_get(body, "on", v, 512) && v[0] == '1';
        if (form_get(body, "uri", v, 512)) {
            if (v[0] && !strstr(v, "://")) {
                snprintf(c->mqtt_uri, sizeof(c->mqtt_uri), "mqtt://%s", v);
            } else {
                strlcpy(c->mqtt_uri, v, sizeof(c->mqtt_uri));
            }
        }
        if (form_get(body, "user", v, 512)) {
            strlcpy(c->mqtt_user, v, sizeof(c->mqtt_user));
        }
        if (form_get(body, "pw", v, 512) && v[0]) {
            strlcpy(c->mqtt_pass, v, sizeof(c->mqtt_pass));
        }
        if (form_get(body, "loc", v, 512) && v[0]) {
            strlcpy(c->mqtt_loc, v, sizeof(c->mqtt_loc));
        }
        if (form_get(body, "dev", v, 512) && v[0]) {
            strlcpy(c->mqtt_dev, v, sizeof(c->mqtt_dev));
        }
        if (c->mqtt_on && strncmp(c->mqtt_uri, "mqtt://", 7) != 0) {
            msg = "Only mqtt:// (plain TCP) is supported";
            ok = false;
        } else if (settings_save(c) != ESP_OK) {
            msg = "Could not save the MQTT settings (storage full?)";
            ok = false;
        } else {
            mqtt_ha_restart();
            msg = c->mqtt_on ? "MQTT saved, connecting (state shown below after a reload)" : "MQTT off";
        }
    } else if (strcmp(section, "svc") == 0) {
        char a[16] = "", r[24] = "";
        form_get(body, "a", a, sizeof(a));
        form_get(body, "r", r, sizeof(r));
        int rt = svc_routine_by_name(r);
        if (strcmp(a, "on") == 0 || strcmp(a, "off") == 0) {
            thermo_svc_enter(a[1] == 'n');
            msg = a[1] == 'n' ? "Service mode on" : "Service mode off";
        } else if (strcmp(a, "run") == 0 && rt >= 0) {
            bool confirm = form_get(body, "confirm", v, 512) && v[0] == '1';
            ok = thermo_svc_start((svc_routine_t)rt, confirm, tmsg, sizeof(tmsg));
            msg = ok ? "Routine started (reload to follow it)" : tmsg;
        } else if (strcmp(a, "cancel") == 0) {
            thermo_svc_cancel();
            msg = "Routine cancelled";
        } else if (strcmp(a, "apply") == 0 && rt >= 0) {
            ok = thermo_svc_apply((svc_routine_t)rt, tmsg, sizeof(tmsg));
            msg = tmsg;
        } else if (strcmp(a, "manual") == 0) {
            char ids[8] = "";
            form_get(body, "id", ids, sizeof(ids));
            form_get(body, "v", v, 512);
            int id = ids[0] ? atoi(ids) : -1;
            bool w = v[0] != 0;
            uint16_t data = !w ? 0 : (strncmp(v, "0x", 2) == 0 ? (uint16_t)strtoul(v, NULL, 16) : ot_to_f88(strtof(v, NULL)));
            ok = id >= 0 && id < 128 && thermo_svc_manual(w, (uint8_t)id, data, tmsg, sizeof(tmsg));
            msg = ok ? "Sent; reload for the answer" : (id < 0 ? "Data-ID missing" : tmsg);
        } else {
            msg = "Unknown service action";
            ok = false;
        }
    } else if (strcmp(section, "cfg") == 0) {
        // Every field of the settings form; only changed values are set
        int changed = 0;
        size_t rn = 0;
        tmsg[0] = 0;
        char cur[24];
        for (int i = 0; i < cfgall_count(); i++) {
            const ct_field_t *f = cfgall_field(i, NULL);
            if (!form_get(body, f->name, v, 512)) {
                if (f->type == CT_BOOL && cfgall_get(i, cur, sizeof(cur)) && strcmp(cur, "0") != 0 &&
                    strstr(body, "cfgform=1")) {
                    cfgall_set(f->name, "0");  // unticked checkbox
                    changed++;
                }
                continue;
            }
            cfgall_get(i, cur, sizeof(cur));
            if (f->type == CT_BOOL) {
                strlcpy(v, "1", 512);
            }
            if (strcmp(cur, v) == 0 || (f->type == CT_FLOAT && fabsf(strtof(cur, NULL) - strtof(v, NULL)) < 1e-4f)) {
                continue;
            }
            if (cfgall_set(f->name, v)) {
                changed++;
            } else if (rn < sizeof(tmsg)) {
                rn += (size_t)snprintf(tmsg + rn, sizeof(tmsg) - rn, "%s%s", rn ? ", " : "Refused: ", f->name);
            }
        }
        if (tmsg[0]) {
            ok = false;
            msg = tmsg;
        } else {
            snprintf(tmsg, sizeof(tmsg), "%d setting(s) changed", changed);
            msg = tmsg;
        }
    } else if (strcmp(section, "prog") == 0) {
        sched_config_t sc;
        thermo_get_sched_config(&sc);
        bool pok = true;
        for (int d = 0; d < 7 && pok; d++) {
            char key[8];
            snprintf(key, sizeof(key), "prog%d", d);
            if (form_get(body, key, v, 512)) {
                pok = sched_prog_parse(&sc, d, v);
            }
        }
        if (pok) {
            thermo_set_sched_config(&sc);
            msg = "Weekly program saved";
        } else {
            msg = "Program not saved: use HH:MM=c or HH:MM=e, up to 6 per day";
            ok = false;
        }
    } else if (strcmp(section, "import") == 0) {
        // The JSON is longer than v: decode it in place from the body
        char *j = strstr(body, "json=");
        if (j) {
            j += 5;
            char *end = strchr(j, '&');
            if (end) {
                *end = 0;
            }
            url_decode(j);
            char rep[96];
            int n = import_config(j, rep, sizeof(rep));
            if (n < 0) {
                snprintf(tmsg, sizeof(tmsg), "Import failed: %s", rep);
                ok = false;
            } else {
                snprintf(tmsg, sizeof(tmsg), "Imported %d item(s). %s", n, rep);
                ok = rep[0] == 0;
            }
            msg = tmsg;
        } else {
            msg = "Nothing to import";
            ok = false;
        }
    } else if (strcmp(section, "svcpin") == 0) {
        form_get(body, "pin", v, 512);
        if (settings_set_svc_pin(c, v)) {
            settings_save(c);
            msg = v[0] ? "Service PIN set" : "Service PIN removed: the device service menu is locked";
        } else {
            msg = "The PIN must be 4 to 6 digits";
            ok = false;
        }
    } else if (strcmp(section, "clock") == 0) {
        c->ntp_on = form_get(body, "ntp", v, 512) && v[0] == '1';
        if (form_get(body, "server", v, 512)) {
            strlcpy(c->ntp_server, v, sizeof(c->ntp_server));
        }
        if (form_get(body, "tz", v, 512) && v[0]) {
            strlcpy(c->tz, v, sizeof(c->tz));
        }
        settings_save(c);
        timekeep_apply_settings();
        struct tm t = {0};
        if (!c->ntp_on && form_get(body, "manual", v, 512) && v[0] &&
            sscanf(v, "%d-%d-%dT%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min) == 5) {
            t.tm_year -= 1900;
            t.tm_mon -= 1;
            timekeep_set_manual(&t);
            msg = "Clock saved and set";
        }
    } else if (strcmp(section, "display") == 0) {
        int a, b;
        if (form_get(body, "bright", v, 512)) {
            c->bright = (uint8_t)fminf(fmaxf(atoi(v), 5), 100);
        }
        if (form_get(body, "dim", v, 512)) {
            c->dim_level = (uint8_t)fminf(fmaxf(atoi(v), 1), 100);
        }
        if (form_get(body, "dim_s", v, 512)) {
            c->dim_after_s = (uint16_t)fminf(fmaxf(atoi(v), 0), 3600);
        }
        if (form_get(body, "home_s", v, 512)) {
            c->home_after_s = (uint16_t)fminf(fmaxf(atoi(v), 0), 3600);
        }
        c->night_on = form_get(body, "night", v, 512) && v[0] == '1';
        if (form_get(body, "n_st", v, 512) && sscanf(v, "%d:%d", &a, &b) == 2) {
            c->night_start = (a % 24) * 60 + b % 60;
        }
        if (form_get(body, "n_en", v, 512) && sscanf(v, "%d:%d", &a, &b) == 2) {
            c->night_end = (a % 24) * 60 + b % 60;
        }
        if (form_get(body, "n_lvl", v, 512)) {
            c->night_level = (uint8_t)fminf(fmaxf(atoi(v), 1), 100);
        }
        if (form_get(body, "off_s", v, 512)) {
            c->off_after_s = (uint16_t)fminf(fmaxf(atoi(v), 0), 3600);
        }
        c->keep_on = form_get(body, "keep", v, 512) && v[0] == '1';
        if (form_get(body, "k_st", v, 512) && sscanf(v, "%d:%d", &a, &b) == 2) {
            c->keep_on_start = (a % 24) * 60 + b % 60;
        }
        if (form_get(body, "k_en", v, 512) && sscanf(v, "%d:%d", &a, &b) == 2) {
            c->keep_on_end = (a % 24) * 60 + b % 60;
        }
        c->motion_wake = form_get(body, "motion", v, 512) && v[0] == '1';
        if (form_get(body, "theme", v, 512)) {
            c->theme_mode = (uint8_t)fminf(fmaxf(atoi(v), 0), 2);
        }
        if (form_get(body, "d_st", v, 512) && sscanf(v, "%d:%d", &a, &b) == 2) {
            c->day_start = (a % 24) * 60 + b % 60;
        }
        if (form_get(body, "d_en", v, 512) && sscanf(v, "%d:%d", &a, &b) == 2) {
            c->day_end = (a % 24) * 60 + b % 60;
        }
        if (form_get(body, "sens", v, 512)) {
            c->motion_sens = (uint8_t)fminf(fmaxf(atoi(v), 0), 2);
        }
        settings_save(c);
    } else if (strcmp(section, "web") == 0) {
        char user[33] = "";
        form_get(body, "user", user, sizeof(user));
        if (form_get(body, "pw", v, 512) && strlen(v) >= 8 && user[0]) {
            settings_set_web_password(c, user, v);
            settings_save(c);
            msg = "Login changed; your browser will ask for it again";
        } else {
            msg = "Username and a password of at least 8 characters are needed";
            ok = false;
        }
    } else {
        msg = "Unknown section";
        ok = false;
    }
    free(v);
    free(c);
    free(body);
    return send_page(req, msg, ok);
}

/* -------------------------------------------------------------------- OTA */

static esp_err_t post_ota(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return httpd_resp_sendstr(req, "No OTA partition (flash this build over USB once first)");
    }
    if (req->content_len <= 0 || req->content_len > part->size) {
        return httpd_resp_sendstr(req, "Firmware file is empty or too large");
    }
    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        return httpd_resp_sendstr(req, "OTA begin failed");
    }
    char *buf = malloc(4096);
    int left = req->content_len;
    ESP_LOGI(TAG, "OTA: %d bytes to %s", left, part->label);
    while (left > 0 && buf) {
        int r = httpd_req_recv(req, buf, left < 4096 ? left : 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, buf, r);
        if (err != ESP_OK) {
            break;
        }
        left -= r;
    }
    free(buf);
    if (err == ESP_OK && left == 0) {
        err = esp_ota_end(ota);
    } else {
        esp_ota_abort(ota);
        if (err == ESP_OK) {
            err = ESP_FAIL;
        }
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(part);
    }
    if (err != ESP_OK) {
        char m[96];
        snprintf(m, sizeof(m), "Install failed: %s (the current firmware stays)", esp_err_to_name(err));
        return httpd_resp_sendstr(req, m);
    }
    httpd_resp_sendstr(req, "Installed. Rebooting into the new firmware; reload this page in ~20 s.");
    esp_timer_start_once(s_restart_timer, 1500 * 1000);
    return ESP_OK;
}

/* -------------------------------------------------------------- startup */

esp_err_t webui_start(void)
{
    if (s_server) {
        return ESP_OK;
    }
    const esp_timer_create_args_t ra = {.callback = restart_cb, .name = "restart"};
    esp_timer_create(&ra, &s_restart_timer);
    const esp_timer_create_args_t wa = {.callback = wifi_cb, .name = "web_wifi"};
    esp_timer_create(&wa, &s_wifi_timer);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 10240;  // HA test (possibly TLS) runs in a handler
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 16;  // default 8; more are registered below
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.core_id = 0;
    cfg.task_priority = 4;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = get_root},
        {.uri = "/api/status", .method = HTTP_GET, .handler = get_status},
        {.uri = "/api/ot", .method = HTTP_GET, .handler = get_ot},
        {.uri = "/api/config", .method = HTTP_GET, .handler = get_config},
        {.uri = "/api/otlog.csv", .method = HTTP_GET, .handler = get_otlog},
        {.uri = "/api/kpi", .method = HTTP_GET, .handler = get_kpi},
        {.uri = "/api/log.csv", .method = HTTP_GET, .handler = get_log},
        {.uri = "/api/service", .method = HTTP_GET, .handler = get_service},
        {.uri = "/save/*", .method = HTTP_POST, .handler = post_save},
        {.uri = "/ota", .method = HTTP_POST, .handler = post_ota},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(s_server, &uris[i]) != ESP_OK) {
            ESP_LOGE(TAG, "could not register %s", uris[i].uri);
        }
    }
    ESP_LOGI(TAG, "web setup on port 80");
    return ESP_OK;
}
