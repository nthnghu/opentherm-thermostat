#include "logring.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static char *s_ring;
static size_t s_head;   // next write position
static bool s_wrapped;
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_prev;
static char s_line[256];  // shared formatting buffer (under s_lock): no extra stack in the logging task

static int hook(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int r = s_prev(fmt, ap);
    // Logging tasks may have small stacks and must never block here: skip the copy when busy.
    if (s_ring && xSemaphoreTake(s_lock, 0) == pdTRUE) {
        int n = vsnprintf(s_line, sizeof(s_line), fmt, ap2);
        if (n > (int)sizeof(s_line) - 1) {
            n = sizeof(s_line) - 1;
        }
        for (int i = 0; i < n; i++) {
            s_ring[s_head++] = s_line[i];
            if (s_head == LOGRING_SIZE) {
                s_head = 0;
                s_wrapped = true;
            }
        }
        xSemaphoreGive(s_lock);
    }
    va_end(ap2);
    return r;
}

void logring_start(void)
{
    s_ring = heap_caps_malloc(LOGRING_SIZE, MALLOC_CAP_SPIRAM);
    s_lock = xSemaphoreCreateMutex();
    if (s_ring && s_lock) {
        s_prev = esp_log_set_vprintf(hook);
    }
}

size_t logring_read(char *out, size_t size)
{
    size_t n = 0;
    if (!s_ring || !size) {
        if (size) {
            out[0] = 0;
        }
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t total = s_wrapped ? LOGRING_SIZE : s_head;
    size_t skip = total > size - 1 ? total - (size - 1) : 0;  // keep the newest part
    size_t start = (s_wrapped ? s_head : 0) + skip;
    for (size_t i = skip; i < total; i++) {
        out[n++] = s_ring[start++ % LOGRING_SIZE];
    }
    xSemaphoreGive(s_lock);
    out[n] = 0;
    return n;
}
