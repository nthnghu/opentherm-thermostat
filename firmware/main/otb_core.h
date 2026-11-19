#pragma once
// OpenTherm master logic, one call per ~1 s bus cycle (docs/research/10-recommendation-phase1.md §3).
// Pure C (no ESP-IDF): the caller supplies the transfer function and the clock, so the host tests can run it
// against the simulated boiler.
//
// Every cycle: ID 0 (status, retried once), ID 1 (control setpoint), then one "slot" frame chosen from a
// schedule of reads/writes (config reads first after a (re)connect, then the most overdue item).
// An ID that answers UNKNOWN-DATAID / DATA-INVALID 3 times is marked unsupported; unsupported IDs are probed
// again hourly and after a reconnect.
#include <stdbool.h>
#include <stdint.h>

#define OTB_FAILS_FOR_LOST 5  // cycles in a row without a status answer

typedef enum { OTB_LINK_UNKNOWN = 0, OTB_LINK_UP, OTB_LINK_LOST } otb_link_t;
typedef enum { OTB_ID_UNKNOWN = 0, OTB_ID_SUPPORTED, OTB_ID_UNSUPPORTED } otb_support_t;
// OTB_XFER_SKIP: the caller's time budget for this cycle is used up; nothing was sent.
typedef enum { OTB_XFER_OK = 0, OTB_XFER_TIMEOUT, OTB_XFER_BAD, OTB_XFER_SKIP } otb_xfer_result_t;

// Transfer one frame; returns OTB_XFER_*. latency_ms may be left untouched.
typedef otb_xfer_result_t (*otb_xfer_fn)(void *ctx, uint32_t req, uint32_t *resp, uint16_t *latency_ms);

typedef struct {
    bool ch_enable;
    bool dhw_enable;
    float tset;          // flow setpoint while CH is enabled
    float dhw_set;       // tank setpoint to write (ID 56), 0 = never write (leave the panel value)
} otb_cmd_t;

// ID 56 write check: after each write the value is read back.
typedef enum {
    OTB_DHWSET_UNKNOWN = 0,  // not written yet
    OTB_DHWSET_OK,           // the read-back matched the written value
    OTB_DHWSET_IGNORED,      // acknowledged, but the boiler kept its own value (e.g. Immergas P7 = 2)
    OTB_DHWSET_REJECTED,     // the write was refused (UNKNOWN-DATAID / DATA-INVALID)
} otb_dhwset_t;

typedef struct {
    bool status_ca;      // Immergas: ID 0 request low byte 0xCA
    bool mirror_57;      // Immergas: write ID 57 = ID 1
    bool explicit_off;   // "off" = CH bit cleared AND ID 1 = 10 °C
} otb_flags_t;

// Boiler values; a value is valid when its bit in .valid is set (bit = OTB_V_*).
enum {
    OTB_V_FLOW = 1u << 0, OTB_V_RET = 1u << 1, OTB_V_MOD = 1u << 2, OTB_V_PRESS = 1u << 3, OTB_V_TDHW = 1u << 4,
    OTB_V_TOUT = 1u << 5, OTB_V_FAULT = 1u << 6, OTB_V_CONFIG = 1u << 7, OTB_V_CAPS = 1u << 8,
    OTB_V_DHW_BOUNDS = 1u << 9, OTB_V_CH_BOUNDS = 1u << 10, OTB_V_DHW_SET = 1u << 11, OTB_V_MAXSET = 1u << 12,
    OTB_V_VERSION = 1u << 13, OTB_V_COUNTERS = 1u << 14, OTB_V_REMOTE = 1u << 15, OTB_V_TEXH = 1u << 16,
    OTB_V_DHW_FLOW = 1u << 17,
};

typedef struct {
    otb_link_t link;
    uint8_t slave_flags;          // ID 0 low byte of the last answer (OT_S_*)
    uint32_t valid;
    float flow_c, return_c, modulation, pressure_bar, tank_c, outside_c, exhaust_c, dhw_flow;
    uint8_t fault_flags, oem_fault;
    uint8_t slave_config, member_id;
    uint8_t max_cap_kw, min_mod_pct;
    int8_t dhw_min, dhw_max, ch_min, ch_max;
    uint16_t remote_params;
    float dhw_set_c, maxset_c;    // last read / acknowledged values
    float slave_ot_version;
    uint8_t slave_type, slave_version;
    uint16_t counters[8];         // IDs 116..123
    float tset_sent;              // last ID 1 value written
    bool tset_acked;              // the last ID 1 write was acknowledged with the same value
    float dhw_set_written;        // last ID 56 value written (0 = none)
    uint8_t dhw_set_verify;       // otb_dhwset_t
    // Statistics
    uint32_t cycles, frames, ok, timeouts, bad, unknown_id, data_invalid, link_losses, retries;
    uint16_t last_latency_ms, max_latency_ms;
    uint32_t latency_sum_ms, latency_n;
    int fails_in_row;
} otb_status_t;

typedef struct {
    uint8_t id;
    bool write;
    uint32_t period_ms;  // 0 = once per (re)connect
    int64_t last_ms;     // INT64_MIN = never
} otb_slot_t;

#define OTB_MAX_SLOTS 40

// One-off frame (service mode): sent instead of the scheduled slot in the next cycle.
typedef struct {
    bool pending;        // waiting to be sent
    uint8_t type, id;    // OT_READ_DATA / OT_WRITE_DATA
    uint16_t data;
    uint32_t seq;        // increments when a request completes
    uint8_t result;      // otb_xfer_result_t of the last completed request
    uint32_t resp;       // its answer (valid with OTB_XFER_OK)
} otb_manual_t;

typedef struct {
    otb_status_t st;
    otb_flags_t flags;
    otb_cmd_t cmd;
    uint8_t support[128];   // otb_support_t
    uint8_t neg[128];       // negative answers in a row
    otb_slot_t slots[OTB_MAX_SLOTS];
    int nslots;
    int64_t last_reprobe_ms;
    float last_57_sent;
    int64_t last_57_ms;
    int64_t dhw_write_ms;   // last ID 56 write (NEVER = not since the (re)connect)
    bool dhw_readback_due;
    otb_manual_t manual;
    bool silent;            // service test: send nothing at all (comm-loss behaviour of the boiler)
} otb_core_t;

void otb_init(otb_core_t *c);
// Another boiler on the bus (simulated <-> real): forget everything learnt from the old one (link, values,
// supported IDs, statistics). The flags, the controller's command and the service settings stay.
void otb_new_boiler(otb_core_t *c);

// The controller hands over a command every second. When it stops (hung or crashed task), the last command must
// not be repeated forever: after OTB_CMD_MAX_AGE_MS heating is switched off. Returns true when it stepped in.
#define OTB_CMD_MAX_AGE_MS 10000
bool otb_cmd_guard(otb_cmd_t *cmd, int64_t age_ms);
// One bus cycle at time now_ms. The transfer function may return OTB_XFER_SKIP to end the cycle early.
void otb_cycle(otb_core_t *c, int64_t now_ms, otb_xfer_fn xfer, void *ctx);
// Queues a one-off frame; false while another one is pending.
bool otb_manual_request(otb_core_t *c, uint8_t type, uint8_t id, uint16_t data);
// Value ID 1 carries for a command (applies explicit_off).
float otb_tset_for(const otb_core_t *c);
const char *otb_link_name(otb_link_t l);
