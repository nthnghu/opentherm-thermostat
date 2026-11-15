#pragma once
// Service mode and commissioning routines (plan B2; docs/research/10-recommendation-phase1.md, "Verify on the
// real boiler"). Pure C (no ESP-IDF), host-tested against the simulated Immergas boiler.
//
// Service mode is entered explicitly (serial / web login; on the device behind a PIN in milestone 6) and ends
// by itself after svc_idle_min without activity. While a routine runs it replaces the normal boiler request
// (CH/DHW/flow setpoint and the bus flags) step by step, reads and writes single Data-IDs, and writes its
// result into the commissioning record (kept in NVS). Normal control resumes when it ends.
// Every routine aborts when the boiler stops answering or reports a fault (except the comm-loss test), when the
// room drops below the frost limit, on its time limit, or when cancelled.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "otb_core.h"

#define SVC_CONFIG_VERSION 1
#define SVC_RECORD_VERSION 2

typedef enum {
    SVC_IDENTITY = 0,  // tests 6 + 16: identity, capabilities, which sensors answer
    SVC_STATUS_CA,     // test 4: do writes need ID 0 low byte 0xCA?
    SVC_WRITES,        // test 8: do ID 56 / 57 writes take effect?
    SVC_SETPOINT,      // test 7: does it heat to ID 1 or to ID 57?
    SVC_CH_OFF,        // tests 9 + 15: does clearing the CH bit stop the burner? pump post-run
    SVC_ID14,          // test 10: is ID 14 (max modulation) honoured?
    SVC_MIN_FIRE,      // test 12: minimum-fire flow (OPV)
    SVC_CUTOUT,        // tests 13 + 14: cut-out overshoot, anti-cycle time, ignition ramp
    SVC_COMM_LOSS,     // tests 18 + 20: E31 after silence, CH after resuming (may need a boiler power cycle!)
    SVC_COUNT
} svc_routine_t;

typedef enum { SVC_R_NONE = 0, SVC_R_DONE, SVC_R_FAILED, SVC_R_ABORTED } svc_rstate_t;

typedef struct {
    uint8_t state;       // svc_rstate_t
    bool sim;            // measured on the simulated boiler (not a real commissioning result)
    int64_t epoch;       // when it ended (0 = clock not set)
    float v[4];          // routine-specific values (see the routine table in service.c)
    char text[200];      // human-readable result and recommendation
} svc_result_t;

typedef struct {
    uint16_t version;
    svc_result_t r[SVC_COUNT];
} svc_record_t;

typedef struct {
    uint16_t version;
    uint16_t idle_min;       // service mode ends after this long without activity
    uint16_t step_min;       // setpoint test: length of each ID 1 step
    uint16_t opv_max_min;    // minimum-fire test: give up after
    uint16_t silence_s;      // comm-loss test: silence length
    uint16_t recover_min;    // comm-loss test: how long to wait for CH after resuming
    float test_flow_c;       // flow setpoint used to get a burn going
} svc_config_t;

typedef struct {
    double now_s;
    int64_t epoch;               // 0 = no clock
    bool sim;                    // the simulated boiler is in use
    const otb_status_t *bus;
    const otb_manual_t *manual;  // state of the one-off frame
    otb_flags_t flags;           // the bus flags in use now
    bool room_ok;
    float room_c, frost_c, flow_max_c;
} svc_in_t;

typedef struct {
    bool active;                 // service mode on
    bool running;                // a routine is in control
    uint8_t routine;             // svc_routine_t (with running)
    char step[80];               // what it is doing now
    int elapsed_s;
    int idle_left_s;             // until service mode ends by itself
    bool override;               // use cmd instead of the normal request
    otb_cmd_t cmd;
    bool set_flags;              // apply flags this step
    otb_flags_t flags;
    bool silent;                 // send nothing (comm-loss test)
    bool manual;                 // queue this one-off frame this step
    uint8_t m_type, m_id;
    uint16_t m_data;
    bool record_changed;         // save the record
    char manual_text[96];        // answer to the last manual read/write
} svc_out_t;

typedef struct {
    svc_config_t cfg;
    svc_record_t rec;
    bool active;
    double active_until;
    int running;                 // -1 = none
    int phase;
    double t0, start_s;
    otb_flags_t saved_flags, flags;
    bool flags_init;             // saved_flags taken (first step of a routine)
    otb_cmd_t cmd;
    bool silent;
    bool confirmed;
    // One-off frame in progress
    bool x_busy;
    uint32_t x_seq;
    int x_tries;
    double x_t;
    uint16_t x_data;
    // User manual request
    bool u_pending, u_busy;
    uint8_t u_type, u_id;
    uint16_t u_data;
    uint32_t u_seq;
    // Abort tracking
    double bad_since;
    // Scratch for the routines
    float f[16];
    int i[8];
    double t[4];
    uint32_t have;               // identity: bit per probed ID answered
    uint16_t data[32];
    svc_out_t out;
} svc_t;

void svc_default_config(svc_config_t *c);
void svc_sanitize(svc_config_t *c);
void svc_init(svc_t *s, const svc_config_t *cfg, const svc_record_t *rec);  // rec may be NULL
void svc_set_config(svc_t *s, const svc_config_t *cfg);

bool svc_enter(svc_t *s, double now_s);
void svc_exit(svc_t *s);  // also cancels a running routine
// Starts a routine (service mode must be on, nothing running). SVC_COMM_LOSS needs confirmed = true.
bool svc_start(svc_t *s, svc_routine_t r, bool confirmed, double now_s, char *why, size_t size);
void svc_cancel(svc_t *s);
// One-off read (any ID) or write (IDs 1, 14, 56, 57 only) while in service mode and no routine runs.
bool svc_manual(svc_t *s, bool write, uint8_t id, uint16_t data, double now_s, char *why, size_t size);
// Call once per second.
const svc_out_t *svc_step(svc_t *s, const svc_in_t *in);

// What a finished routine recommends (svc apply).
typedef struct {
    bool set_ca, ca;
    bool set_mirror, mirror;
    bool set_explicit_off, explicit_off;
    bool set_flow_min;
    float flow_min_c;
} svc_apply_t;
bool svc_recommend(const svc_record_t *rec, svc_routine_t r, svc_apply_t *a);

const char *svc_routine_name(svc_routine_t r);   // short id, e.g. "min_fire"
const char *svc_routine_title(svc_routine_t r);
const char *svc_routine_help(svc_routine_t r);   // what it does, tests covered, how long, gas burned
int svc_routine_by_name(const char *name);       // -1 = unknown
const char *svc_rstate_name(svc_rstate_t s);

int svc_config_count(void);
const char *svc_config_name(int idx);
const char *svc_config_help(int idx);
bool svc_config_get(const svc_config_t *c, int idx, char *out, size_t size);
bool svc_config_set(svc_config_t *c, const char *name, const char *value);
