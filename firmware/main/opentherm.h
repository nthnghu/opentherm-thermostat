#pragma once
// OpenTherm master PHY over the DIYLESS Master OpenTherm Shield.
// GPIO29 = shield output -> ESP (in), GPIO30 = ESP -> shield (out).
// Manchester at 1 kbit/s: GPTimer drives TX, a GPIO edge ISR decodes RX.
// Only the bus task (ot_bus.c) calls ot_transfer().
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ot_frame.h"

#define OT_PIN_IN   29
#define OT_PIN_OUT  30

esp_err_t ot_init(void);
// Sends a request and waits for the response (up to 800 ms). ESP_ERR_TIMEOUT when no boiler answers.
// latency_ms (optional): end of our stop bit to the boiler's start bit.
esp_err_t ot_transfer(uint32_t request, uint32_t *response, uint16_t *latency_ms);
int ot_in_level(void);  // raw level of the shield's output line
