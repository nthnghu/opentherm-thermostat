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
          "\"hysteresis\":%.2f,\"time_source\":\"%s\",\"history\":[",
          st.boiler_sim ? "true" : "false", st.boiler_online ? "true" : "false", st.flame ? "true" : "false", st.flow_c,
          st.modulation, st.offset, st.hysteresis, timekeep_source_name());
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

static esp_err_t post_save(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    char *body = read_body(req, 4096);
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
        {.uri = "/save/*", .method = HTTP_POST, .handler = post_save},
        {.uri = "/ota", .method = HTTP_POST, .handler = post_ota},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    ESP_LOGI(TAG, "web setup on port 80");
    return ESP_OK;
}
