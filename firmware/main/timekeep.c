#include "timekeep.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "settings.h"

static const char *TAG = "time";

// Survives a software reset (the system time itself is kept by the RTC timer across soft resets).
static RTC_NOINIT_ATTR uint32_t s_rtc_src_magic;
static time_src_t s_src;
static bool s_sntp_running;

#define SRC_MAGIC 0x54494D00u

static void set_src(time_src_t s)
{
    s_src = s;
    s_rtc_src_magic = SRC_MAGIC | s;
}

static void sntp_sync_cb(struct timeval *tv)
{
    set_src(TIME_SRC_NTP);
    ESP_LOGI(TAG, "NTP sync");
}

void timekeep_apply_settings(void)
{
    // A copy taken under the settings lock (not the live struct, which a save may be writing), on the heap: the
    // struct is 2.5 KB and this is called from tasks with small stacks.
    settings_t *cp = malloc(sizeof(settings_t));
    if (!cp) {
        return;
    }
    settings_copy(cp);
    setenv("TZ", cp->tz, 1);
    tzset();
    bool ntp_on = cp->ntp_on;
    char ntp_server[sizeof(cp->ntp_server)];
    strlcpy(ntp_server, cp->ntp_server, sizeof(ntp_server));
    free(cp);
    if (s_sntp_running) {
        esp_netif_sntp_deinit();
        s_sntp_running = false;
    }
    if (ntp_on) {
        static char server[64];  // SNTP keeps the pointer
        strlcpy(server, ntp_server[0] ? ntp_server : "pool.ntp.org", sizeof(server));
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
        cfg.sync_cb = sntp_sync_cb;
        cfg.start = true;  // retries on its own until the network is up
        if (esp_netif_sntp_init(&cfg) == ESP_OK) {
            s_sntp_running = true;
            ESP_LOGI(TAG, "SNTP on (%s)", server);
        }
    }
}

void timekeep_init(void)
{
    if ((s_rtc_src_magic & 0xFFFFFF00u) == SRC_MAGIC && timekeep_valid()) {
        s_src = s_rtc_src_magic & 0xFF;  // clock kept across a soft reset
    } else {
        s_src = TIME_SRC_NONE;
    }
    timekeep_apply_settings();
}

bool timekeep_valid(void)
{
    return time(NULL) > 1735689600;  // after 2025-01-01
}

time_src_t timekeep_source(void)
{
    return timekeep_valid() ? s_src : TIME_SRC_NONE;
}

const char *timekeep_source_name(void)
{
    switch (timekeep_source()) {
    case TIME_SRC_NTP:
        return "NTP";
    case TIME_SRC_MANUAL:
        return "manual";
    default:
        return "not set";
    }
}

void timekeep_set_manual(const struct tm *local)
{
    struct tm t = *local;
    t.tm_isdst = -1;
    time_t v = mktime(&t);
    struct timeval tv = {.tv_sec = v};
    settimeofday(&tv, NULL);
    set_src(TIME_SRC_MANUAL);
    ESP_LOGI(TAG, "clock set by hand");
}
