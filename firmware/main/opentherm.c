#include "opentherm.h"
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

typedef enum { ST_IDLE, ST_SENDING, ST_WAITING, ST_START_BIT, ST_RECEIVING, ST_READY, ST_INVALID } ot_state_t;

static gptimer_handle_t s_timer;
static SemaphoreHandle_t s_done;
static volatile ot_state_t s_state;
static uint8_t s_tx_levels[68];  // 34 bits x 2 half-bits
static volatile int s_tx_pos;
static volatile int64_t s_rx_ts;
static volatile int s_rx_bits;
static volatile uint32_t s_rx_frame;
static volatile int64_t s_tx_end, s_rx_start;
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
    s_rx_ts = esp_timer_get_time();
    s_tx_end = s_rx_ts;
    s_state = ST_WAITING;
    return false;
}

// Response decoding, after the reference Arduino OpenTherm library (ihormelnyk): bit value is the
// inverted line level sampled at the first edge more than 750 us after the previous mid-bit edge.
static void IRAM_ATTR rx_edge(void *arg)
{
    int64_t now = esp_timer_get_time();
    int level = gpio_get_level(OT_PIN_IN);
    BaseType_t woke = pdFALSE;
    switch (s_state) {
    case ST_WAITING:
        if (level == 1) {
            s_state = ST_START_BIT;
            s_rx_ts = now;
            s_rx_start = now;
        } else {
            s_state = ST_INVALID;
            xSemaphoreGiveFromISR(s_done, &woke);
        }
        break;
    case ST_START_BIT:
        if (now - s_rx_ts < 750 && level == 0) {
            s_state = ST_RECEIVING;
            s_rx_ts = now;
            s_rx_bits = 0;
            s_rx_frame = 0;
        } else {
            s_state = ST_INVALID;
            xSemaphoreGiveFromISR(s_done, &woke);
        }
        break;
    case ST_RECEIVING:
        if (now - s_rx_ts > 750) {
            if (s_rx_bits < 32) {
                s_rx_frame = (s_rx_frame << 1) | (level ? 0 : 1);
                s_rx_ts = now;
                s_rx_bits++;
            } else {
                s_state = ST_READY;  // stop bit
                xSemaphoreGiveFromISR(s_done, &woke);
            }
        }
        break;
    default:
        break;
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
    // Manchester: '1' = active then idle, '0' = idle then active. Start and stop bits are '1'.
    int p = 0;
    for (int i = -1; i <= 32; i++) {
        bool bit = (i < 0 || i == 32) ? true : (request >> (31 - i)) & 1;
        s_tx_levels[p++] = bit ? OUT_ACTIVE : OUT_IDLE;
        s_tx_levels[p++] = bit ? OUT_IDLE : OUT_ACTIVE;
    }
    xSemaphoreTake(s_done, 0);
    s_tx_pos = 0;
    s_state = ST_SENDING;
    gptimer_set_raw_count(s_timer, 0);
    gptimer_start(s_timer);
    // 34 ms of TX, then the boiler answers within 20..800 ms.
    bool got = xSemaphoreTake(s_done, pdMS_TO_TICKS(34 + 800 + 40)) == pdTRUE;
    ot_state_t st = s_state;
    s_state = ST_IDLE;
    if (!got) {
        if (st == ST_SENDING) {
            gptimer_stop(s_timer);
            gpio_set_level(OT_PIN_OUT, OUT_IDLE);
        }
        return ESP_ERR_TIMEOUT;
    }
    if (st != ST_READY) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *response = s_rx_frame;
    if (latency_ms) {
        int64_t l = (s_rx_start - s_tx_end) / 1000;
        *latency_ms = l < 0 ? 0 : (l > 65535 ? 65535 : (uint16_t)l);
    }
    return ot_parity_ok(*response) ? ESP_OK : ESP_ERR_INVALID_CRC;
}
