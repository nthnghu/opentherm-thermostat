#include "portal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "settings.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <fcntl.h>
#include <errno.h>

static const char *TAG = "portal";
static httpd_handle_t s_server;
static char s_last_result[128];

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

// Sends text with &, <, >, " escaped.
static void send_escaped(httpd_req_t *req, const char *s)
{
    char buf[64];
    size_t n = 0;
    for (; *s; s++) {
        const char *rep = NULL;
        switch (*s) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
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

#define SEND(req, str) httpd_resp_send_chunk(req, str, HTTPD_RESP_USE_STRLEN)

static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>P4 Test setup</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:520px;margin:0 auto;padding:16px;background:#111a22;color:#e6edf3}"
    "h1{font-size:22px}label{display:block;margin:16px 0 6px;font-weight:600}"
    "input,textarea{width:100%;box-sizing:border-box;padding:10px;font-size:16px;border-radius:8px;border:1px solid #455a64;background:#1e2a38;color:#e6edf3}"
    "textarea{height:120px;font-family:monospace;font-size:13px}"
    "button{margin-top:20px;width:100%;padding:14px;font-size:17px;border:0;border-radius:8px;background:#1565c0;color:#fff}"
    ".hint{color:#90a4ae;font-size:14px}.ok{color:#81c784}.err{color:#e57373}a{color:#4fc3f7}"
    "</style></head><body><h1>Home Assistant access</h1>";

/* ---------------------------------------------------------------- HA test */

bool portal_test_ha(char *msg, size_t msg_size)
{
    const settings_t *cfg = settings_get();
    bool ok = false;
    if (!cfg->ha_url[0] || !cfg->ha_token[0]) {
        snprintf(msg, msg_size, "HA URL or token missing");
        goto out;
    }
    char url[160];
    snprintf(url, sizeof(url), "%s/api/", cfg->ha_url);
    char *auth = malloc(strlen(cfg->ha_token) + 8);
    sprintf(auth, "Bearer %s", cfg->ha_token);

    esp_http_client_config_t hc = {
        .url = url,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    esp_http_client_set_header(c, "Authorization", auth);
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    free(auth);

    if (err != ESP_OK) {
        snprintf(msg, msg_size, "Can't reach %s (%s)", cfg->ha_url, esp_err_to_name(err));
    } else if (status == 200) {
        snprintf(msg, msg_size, "Connected to Home Assistant");
        ok = true;
    } else if (status == 401) {
        snprintf(msg, msg_size, "HA rejected the token (401)");
    } else {
        snprintf(msg, msg_size, "HA answered HTTP %d", status);
    }
out:
    strlcpy(s_last_result, msg, sizeof(s_last_result));
    ESP_LOGI(TAG, "HA test: %s", msg);
    return ok;
}

const char *portal_last_result(void)
{
    return s_last_result;
}


/* ------------------------------------------------------------- diagnostics */

// TCP connect test. Returns a short result: "open (12 ms)", "refused (host reachable)", "timeout", ...
static void tcp_probe(const char *host, int port, char *out, size_t n)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
    char port_s[8];
    snprintf(port_s, sizeof(port_s), "%d", port);
    if (getaddrinfo(host, port_s, &hints, &res) != 0 || !res) {
        snprintf(out, n, "DNS lookup failed");
        return;
    }
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(sock, F_SETFL, O_NONBLOCK);
    int64_t t0 = esp_timer_get_time();
    int r = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r != 0 && errno != EINPROGRESS) {
        snprintf(out, n, "connect error %d (%s)", errno, strerror(errno));
        close(sock);
        return;
    }
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(sock, &wf);
    struct timeval tv = {.tv_sec = 4};
    r = select(sock + 1, NULL, &wf, NULL, &tv);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (r <= 0) {
        snprintf(out, n, "TIMEOUT after %d ms (no answer: dropped by a firewall or no route)", ms);
    } else {
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err == 0) {
            snprintf(out, n, "OPEN (%d ms)", ms);
        } else if (err == ECONNREFUSED) {
            snprintf(out, n, "REFUSED in %d ms (host reachable, port closed)", ms);
        } else {
            snprintf(out, n, "error %d (%s) after %d ms", err, strerror(err), ms);
        }
    }
    close(sock);
}

// GET /diag[?host=1.2.3.4&port=80] - plain-text network report.
static esp_err_t get_diag(httpd_req_t *req)
{
    char line[200], q[128] = "", host[64] = "", port_s[8] = "";
    httpd_resp_set_type(req, "text/plain");

    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    esp_netif_dns_info_t dns = {0};
    if (nif) {
        esp_netif_get_ip_info(nif, &ip);
        esp_netif_get_dns_info(nif, ESP_NETIF_DNS_MAIN, &dns);
    }
    snprintf(line, sizeof(line), "ip      " IPSTR "\nnetmask " IPSTR "\ngateway " IPSTR "\ndns     " IPSTR "\n\n",
             IP2STR(&ip.ip), IP2STR(&ip.netmask), IP2STR(&ip.gw), IP2STR(&dns.ip.u_addr.ip4));
    SEND(req, line);

    char result[120];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "host", host, sizeof(host)) == ESP_OK) {
        int port = 80;
        if (httpd_query_key_value(q, "port", port_s, sizeof(port_s)) == ESP_OK) {
            port = atoi(port_s);
        }
        tcp_probe(host, port, result, sizeof(result));
        snprintf(line, sizeof(line), "tcp %s:%d -> %s\n", host, port, result);
        SEND(req, line);
    } else {
        // Default: the gateway, then the saved Home Assistant URL.
        char gw[16];
        snprintf(gw, sizeof(gw), IPSTR, IP2STR(&ip.gw));
        tcp_probe(gw, 80, result, sizeof(result));
        snprintf(line, sizeof(line), "tcp gateway %s:80 -> %s\n", gw, result);
        SEND(req, line);

        const char *u = settings_get()->ha_url;
        const char *h = strstr(u, "://");
        h = h ? h + 3 : u;
        int port = strncmp(u, "https", 5) == 0 ? 443 : 80;
        size_t hl = strcspn(h, ":/");
        snprintf(host, sizeof(host), "%.*s", (int)(hl < sizeof(host) ? hl : sizeof(host) - 1), h);
        if (h[hl] == ':') {
            port = atoi(h + hl + 1);
        }
        tcp_probe(host, port, result, sizeof(result));
        snprintf(line, sizeof(line), "tcp HA %s:%d -> %s\n", host, port, result);
        SEND(req, line);
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* --------------------------------------------------------------- handlers */

static esp_err_t send_form(httpd_req_t *req, const char *result, bool ok)
{
    const settings_t *cfg = settings_get();
    httpd_resp_set_type(req, "text/html");
    SEND(req, PAGE_HEAD);
    if (result) {
        SEND(req, ok ? "<p class=ok>&#10004; " : "<p class=err>&#10008; ");
        send_escaped(req, result);
        SEND(req, ok ? "</p><p class=hint>All set. You can close this page; the Sensors app now uses these settings.</p>"
                     : "</p>");
    }
    SEND(req, "<form method=post action=/save>"
              "<label for=url>Home Assistant URL</label>"
              "<input id=url name=url autocapitalize=off autocorrect=off placeholder='http://192.168.1.10:8123' value=\"");
    send_escaped(req, cfg->ha_url);
    SEND(req, "\"><p class=hint>As you open it on your local network, e.g. http://homeassistant.local:8123</p>"
              "<label for=token>Long-lived access token</label>"
              "<textarea id=token name=token autocapitalize=off autocorrect=off spellcheck=false placeholder='");
    SEND(req, cfg->ha_token[0] ? "A token is saved. Paste a new one to replace it." : "Paste the token here");
    SEND(req, "'></textarea><p class=hint>In Home Assistant: your profile &rarr; Security &rarr; "
              "Long-lived access tokens &rarr; Create token.");
    if (cfg->ha_url[0]) {
        SEND(req, " <a target=_blank href=\"");
        send_escaped(req, cfg->ha_url);
        SEND(req, "/profile/security\">Open that page</a>");
    }
    SEND(req, "</p><button type=submit>Save and test</button></form></body></html>");
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t get_root(httpd_req_t *req)
{
    return send_form(req, NULL, false);
}

static esp_err_t post_save(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 2048) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
    }
    char *body = calloc(1, len + 1);
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += r;
    }

    settings_t s = *settings_get();
    char *val = malloc(1024);
    if (httpd_query_key_value(body, "url", val, 1024) == ESP_OK) {
        url_decode(val);
        trim(val);
        size_t n = strlen(val);
        if (n && val[n - 1] == '/') {
            val[n - 1] = 0;
        }
        if (val[0] && strncmp(val, "http://", 7) != 0 && strncmp(val, "https://", 8) != 0) {
            // Accept "192.168.1.10:8123" and add the scheme.
            char tmp[160];
            snprintf(tmp, sizeof(tmp), "http://%s", val);
            strlcpy(val, tmp, 1024);
        }
        strlcpy(s.ha_url, val, sizeof(s.ha_url));
    }
    if (httpd_query_key_value(body, "token", val, 1024) == ESP_OK) {
        url_decode(val);
        trim(val);
        if (val[0]) {  // empty keeps the saved token
            strlcpy(s.ha_token, val, sizeof(s.ha_token));
        }
    }
    free(val);
    free(body);

    if (settings_save(&s) != ESP_OK) {
        return send_form(req, "Could not save settings", false);
    }
    char msg[128];
    bool ok = portal_test_ha(msg, sizeof(msg));
    return send_form(req, msg, ok);
}

esp_err_t portal_start(void)
{
    if (s_server) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 10240;  // HA test request (TLS) runs in the handler
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = get_root};
    const httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = post_save};
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &save);
    const httpd_uri_t diag = {.uri = "/diag", .method = HTTP_GET, .handler = get_diag};
    httpd_register_uri_handler(s_server, &diag);
    ESP_LOGI(TAG, "setup page running");
    return ESP_OK;
}

void portal_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "setup page stopped");
    }
}

bool portal_running(void)
{
    return s_server != NULL;
}
