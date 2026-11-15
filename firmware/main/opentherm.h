#pragma once
// OpenTherm master over the DIYLESS Master OpenTherm Shield.
// GPIO29 = shield output -> ESP (in), GPIO30 = ESP -> shield (out).
// Manchester at 1 kbit/s: GPTimer drives TX, a GPIO edge ISR decodes RX.
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define OT_PIN_IN   29
#define OT_PIN_OUT  30

// Message types
#define OT_READ_DATA        0
#define OT_WRITE_DATA       1
#define OT_READ_ACK         4
#define OT_WRITE_ACK        5
#define OT_DATA_INVALID     6
#define OT_UNKNOWN_DATA_ID  7

// Data IDs used here
#define OT_ID_STATUS        0
#define OT_ID_TSET          1
#define OT_ID_SLAVE_CONFIG  3
#define OT_ID_REL_MOD       17
#define OT_ID_TBOILER       25

esp_err_t ot_init(void);
// Simulated boiler instead of the shield (no GPIO traffic).
void ot_set_simulated(bool sim);
bool ot_simulated(void);
// Sends a request and waits for the response (up to 800 ms). ESP_ERR_TIMEOUT when no boiler answers.
esp_err_t ot_transfer(uint32_t request, uint32_t *response);
int ot_in_level(void);  // raw level of the shield's output line

uint32_t ot_frame(uint8_t type, uint8_t id, uint16_t data);
bool ot_parity_ok(uint32_t frame);
static inline uint8_t ot_type(uint32_t f) { return (f >> 28) & 7; }
static inline uint8_t ot_id(uint32_t f) { return (f >> 16) & 0xFF; }
static inline uint16_t ot_data(uint32_t f) { return f & 0xFFFF; }
static inline float ot_f88(uint16_t d) { return (int16_t)d / 256.0f; }
static inline uint16_t ot_to_f88(float v) { return (uint16_t)(int16_t)(v * 256.0f); }

// Simulated boiler (ot_sim.c)
uint32_t ot_sim_respond(uint32_t request);
void ot_sim_tick(float dt_s);
