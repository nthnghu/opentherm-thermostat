#include "logring.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// The ring sits in PSRAM that a restart does not clear, so after a watchdog restart the previous run's log can
// still be read (/api/log?prev=1). After power-on the memory is random: the magic and the head check reject it.
#define MAGIC 0x4C4F4752u  // "LOGR"
typedef struct {
    uint32_t magic;
    uint32_t head;     // next write position
    uint32_t wrapped;
    uint32_t check;    // magic ^ head ^ wrapped
} ring_hdr_t;

EXT_RAM_NOINIT_ATTR static ring_hdr_t s_hdr;
EXT_RAM_NOINIT_ATTR static char s_ring[LOGRING_SIZE];
static bool s_on;
static char *s_prev;      // the previous run's log, oldest first (PSRAM heap), or NULL
static size_t s_prev_len;
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_prev_vprintf;
static char s_line[256];  // shared formatting buffer (under s_lock): no extra stack in the logging task

static void seal(void)
{
    s_hdr.check = s_hdr.magic ^ s_hdr.head ^ s_hdr.wrapped;
}

static int hook(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int r = s_prev_vprintf(fmt, ap);
    // Logging tasks may have small stacks and must never block here: skip the copy when busy.
    if (s_on && xSemaphoreTake(s_lock, 0) == pdTRUE) {
        int n = vsnprintf(s_line, sizeof(s_line), fmt, ap2);
        if (n > (int)sizeof(s_line) - 1) {
            n = sizeof(s_line) - 1;
        }
        uint32_t h = s_hdr.head;
        for (int i = 0; i < n; i++) {
            s_ring[h++] = s_line[i];
            if (h == LOGRING_SIZE) {
                h = 0;
                s_hdr.wrapped = 1;
            }
        }
        s_hdr.head = h;
        seal();
        xSemaphoreGive(s_lock);
    }
    va_end(ap2);
    return r;
}

// Writes the cached ring back to PSRAM before a software restart, so the newest lines survive it.
static void flush(void)
{
    esp_cache_msync(&s_hdr, sizeof(s_hdr), ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    esp_cache_msync(s_ring, sizeof(s_ring), ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static size_t copy_out(char *out, size_t size, uint32_t head, bool wrapped)
{
    size_t total = wrapped ? LOGRING_SIZE : head, n = 0;
    size_t skip = total > size - 1 ? total - (size - 1) : 0;  // keep the newest part
    size_t start = (wrapped ? head : 0) + skip;
    for (size_t i = skip; i < total; i++) {
        char ch = s_ring[start++ % LOGRING_SIZE];
        out[n++] = ch ? ch : ' ';
    }
    out[n] = 0;
    return n;
}

void logring_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return;
    }
    bool valid = s_hdr.magic == MAGIC && s_hdr.head < LOGRING_SIZE && s_hdr.wrapped <= 1 &&
                 s_hdr.check == (s_hdr.magic ^ s_hdr.head ^ s_hdr.wrapped);
    esp_reset_reason_t why = esp_reset_reason();
    if (valid && why != ESP_RST_POWERON && why != ESP_RST_BROWNOUT) {
        size_t len = s_hdr.wrapped ? LOGRING_SIZE : s_hdr.head;
        s_prev = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
        if (s_prev) {
            s_prev_len = copy_out(s_prev, len + 1, s_hdr.head, s_hdr.wrapped);
        }
    }
    s_hdr.magic = MAGIC;
    s_hdr.head = 0;
    s_hdr.wrapped = 0;
    seal();
    s_on = true;
    s_prev_vprintf = esp_log_set_vprintf(hook);
    esp_register_shutdown_handler(flush);
}

size_t logring_read(char *out, size_t size)
{
    if (!size) {
        return 0;
    }
    if (!s_on) {
        out[0] = 0;
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = copy_out(out, size, s_hdr.head, s_hdr.wrapped);
    xSemaphoreGive(s_lock);
    return n;
}

size_t logring_read_prev(char *out, size_t size)
{
    if (!size) {
        return 0;
    }
    size_t n = 0;
    if (s_prev) {
        n = s_prev_len < size - 1 ? s_prev_len : size - 1;
        memcpy(out, s_prev + (s_prev_len - n), n);  // the newest part
    }
    out[n] = 0;
    return n;
}
