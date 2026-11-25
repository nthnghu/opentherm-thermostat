#pragma once
// OpenTherm master PHY over the DIYLESS Master OpenTherm Shield.
// GPIO51 = shield output -> ESP (in), GPIO52 = ESP -> shield (out) (header top row: spare pins, closer to the shield).
// Manchester at 1 kbit/s: GPTimer drives TX, a GPIO edge ISR decodes RX.
// Only the bus task (ot_bus.c) calls ot_transfer().
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ot_frame.h"

#define OT_PIN_IN   51
#define OT_PIN_OUT  52

esp_err_t ot_init(void);
// Sends a request and waits for the response (up to 800 ms). ESP_ERR_TIMEOUT when no boiler answers.
// latency_ms (optional): end of our stop bit to the boiler's start bit.
esp_err_t ot_transfer(uint32_t request, uint32_t *response, uint16_t *latency_ms);
int ot_in_level(void);  // raw level of the shield's output line
uint32_t ot_in_edges(void);  // input edges counted since the start (inside and outside frames)
// Line test only (console "OT line"), with the bus paused: holds our output active (line voltage low) or idle.
void ot_line_hold(bool active);
// Pin finding only (console "OT find in", shield data wires off): drives the INPUT pin as an output, then back.
void ot_in_probe(bool on, int level);
