#pragma once
// OpenTherm frame helpers and Data-IDs. Pure C (no ESP-IDF), so it also builds in the host tests.
#include <stdbool.h>
#include <stdint.h>

// Message types
#define OT_READ_DATA        0
#define OT_WRITE_DATA       1
#define OT_INVALID_DATA     2
#define OT_READ_ACK         4
#define OT_WRITE_ACK        5
#define OT_DATA_INVALID     6
#define OT_UNKNOWN_DATA_ID  7

// Data-IDs
#define OT_ID_STATUS            0
#define OT_ID_TSET              1
#define OT_ID_MASTER_CONFIG     2
#define OT_ID_SLAVE_CONFIG      3
#define OT_ID_FAULT             5
#define OT_ID_REMOTE_PARAMS     6
#define OT_ID_MAX_REL_MOD       14
#define OT_ID_MAX_CAP_MIN_MOD   15
#define OT_ID_TROOM_SET         16
#define OT_ID_REL_MOD           17
#define OT_ID_CH_PRESSURE       18
#define OT_ID_DHW_FLOW          19
#define OT_ID_TROOM             24
#define OT_ID_TBOILER           25
#define OT_ID_TDHW              26
#define OT_ID_TOUTSIDE          27
#define OT_ID_TRET              28
#define OT_ID_TEXHAUST          33
#define OT_ID_FAN_SPEED         35  // hb setpoint, lb actual (rpm / 60)
#define OT_ID_FLAME_CURRENT     36  // f8.8 uA
#define OT_ID_TDHW_BOUNDS       48
#define OT_ID_MAXTSET_BOUNDS    49
#define OT_ID_TDHW_SET          56
#define OT_ID_MAXTSET           57
#define OT_ID_OEM_DIAG          115
#define OT_ID_BURNER_STARTS     116
#define OT_ID_CH_PUMP_STARTS    117
#define OT_ID_DHW_PUMP_STARTS   118
#define OT_ID_DHW_BURNER_STARTS 119
#define OT_ID_BURNER_HOURS      120
#define OT_ID_CH_PUMP_HOURS     121
#define OT_ID_DHW_PUMP_HOURS    122
#define OT_ID_DHW_BURNER_HOURS  123
#define OT_ID_MASTER_OT_VERSION 124
#define OT_ID_SLAVE_OT_VERSION  125
#define OT_ID_MASTER_VERSION    126
#define OT_ID_SLAVE_VERSION     127

// ID 0: master flags (high byte of the request)
#define OT_M_CH_ENABLE   0x01
#define OT_M_DHW_ENABLE  0x02
// ID 0: slave flags (low byte of the response)
#define OT_S_FAULT       0x01
#define OT_S_CH_ACTIVE   0x02
#define OT_S_DHW_ACTIVE  0x04
#define OT_S_FLAME       0x08

// Low byte of the ID 0 request that Immergas boilers expect (OTGateway "Immergas fix").
#define OT_IMMERGAS_STATUS_LB 0xCA

uint32_t ot_frame(uint8_t type, uint8_t id, uint16_t data);
bool ot_parity_ok(uint32_t frame);
const char *ot_type_name(uint8_t type);

static inline uint8_t ot_type(uint32_t f) { return (f >> 28) & 7; }
static inline uint8_t ot_id(uint32_t f) { return (f >> 16) & 0xFF; }
static inline uint16_t ot_data(uint32_t f) { return f & 0xFFFF; }
static inline uint8_t ot_hb(uint16_t d) { return d >> 8; }
static inline uint8_t ot_lb(uint16_t d) { return d & 0xFF; }
static inline float ot_f88(uint16_t d) { return (int16_t)d / 256.0f; }
static inline uint16_t ot_to_f88(float v)
{
    float s = v * 256.0f;
    return (uint16_t)(int16_t)(s < 0 ? s - 0.5f : s + 0.5f);
}
