#include "opentherm.h"
#include "ot_manchester.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "ot";

// The shield inverts: out LOW = line active (current), HIGH = idle. On the input, HIGH = active.
#define OUT_ACTIVE 0
#define OUT_IDLE   1

// Who owns the line: the TX timer (SENDING), the RX edge ISR (RECEIVING, decoding into s_rx) or nobody (IDLE)
typedef enum { ST_IDLE, ST_SENDING, ST_RECEIVING } ot_state_t;

static gptimer_handle_t s_timer;
static SemaphoreHandle_t s_done;
static volatile ot_state_t s_state;
static uint8_t s_tx_levels[OTM_HALF_BITS];
static volatile int s_tx_pos;
static otm_rx_t s_rx;
static volatile int64_t s_tx_end;
static bool s_inited;

static bool IRAM_ATTR tx_alarm(gptimer_handle_t t, const gptimer_alarm_event_data_t *e, void *arg)
{
    int pos = s_tx_pos;
    if (pos < (int)sizeof(s_tx_levels)) {
        gpio_set_level(OT_PIN_OUT, s_tx_levels[pos]);
        s_tx_pos = pos + 1;
        return false;
    }
    gpio_set_level(OT_PIN_OUT, OUT_IDLE);
    gptimer_stop(t);
    s_tx_end = esp_timer_get_time();
    otm_rx_begin(&s_rx);
    s_state = ST_RECEIVING;
    return false;
}

// Response decoding: ot_manchester.h (inlined here, so it stays in IRAM)
static void IRAM_ATTR rx_edge(void *arg)
{
    if (s_state != ST_RECEIVING) {
        return;
    }
    BaseType_t woke = pdFALSE;
    if (otm_rx_edge(&s_rx, esp_timer_get_time(), gpio_get_level(OT_PIN_IN))) {
        s_state = ST_IDLE;
        xSemaphoreGiveFromISR(s_done, &woke);
    }
    if (woke) {
        portYIELD_FROM_ISR();
    }
}

esp_err_t ot_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_done = xSemaphoreCreateBinary();
    gpio_config_t out = {.pin_bit_mask = 1ULL << OT_PIN_OUT, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&out);
    gpio_set_level(OT_PIN_OUT, OUT_IDLE);
    gpio_config_t in = {.pin_bit_mask = 1ULL << OT_PIN_IN, .mode = GPIO_MODE_INPUT, .intr_type = GPIO_INTR_ANYEDGE};
    gpio_config(&in);
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL3);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    gpio_isr_handler_add(OT_PIN_IN, rx_edge, NULL);

    gptimer_config_t tc = {.clk_src = GPTIMER_CLK_SRC_DEFAULT, .direction = GPTIMER_COUNT_UP, .resolution_hz = 1000000};
    err = gptimer_new_timer(&tc, &s_timer);  // not fatal: without the bus the link alert reports it
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "timer: %s", esp_err_to_name(err));
        return err;
    }
    gptimer_event_callbacks_t cbs = {.on_alarm = tx_alarm};
    gptimer_register_event_callbacks(s_timer, &cbs, NULL);
    gptimer_alarm_config_t ac = {.alarm_count = 500, .reload_count = 0, .flags.auto_reload_on_alarm = true};
    gptimer_set_alarm_action(s_timer, &ac);
    gptimer_enable(s_timer);
    s_state = ST_IDLE;
    s_inited = true;
    ESP_LOGI(TAG, "OpenTherm PHY ready (in GPIO%d, out GPIO%d)", OT_PIN_IN, OT_PIN_OUT);
    return ESP_OK;
}

int ot_in_level(void)
{
    return gpio_get_level(OT_PIN_IN);
}

esp_err_t ot_transfer(uint32_t request, uint32_t *response, uint16_t *latency_ms)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    otm_encode(request, s_tx_levels, OUT_ACTIVE, OUT_IDLE);
    xSemaphoreTake(s_done, 0);
    s_tx_pos = 0;
    s_state = ST_SENDING;
    gptimer_set_raw_count(s_timer, 0);
    gptimer_start(s_timer);
    // 34 ms of TX, then the boiler answers within 20..800 ms.
    bool got = xSemaphoreTake(s_done, pdMS_TO_TICKS(34 + 800 + 40)) == pdTRUE;
    ot_state_t st = s_state;
    s_state = ST_IDLE;
    otm_state_t rx = s_rx.st;
    if (!got) {
        if (st == ST_SENDING) {
            gptimer_stop(s_timer);
            gpio_set_level(OT_PIN_OUT, OUT_IDLE);
        }
        return ESP_ERR_TIMEOUT;
    }
    if (rx != OTM_READY) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *response = s_rx.frame;
    if (latency_ms) {
        int64_t l = (s_rx.start - s_tx_end) / 1000;
        *latency_ms = l < 0 ? 0 : (l > 65535 ? 65535 : (uint16_t)l);
    }
    return ot_parity_ok(*response) ? ESP_OK : ESP_ERR_INVALID_CRC;
}
