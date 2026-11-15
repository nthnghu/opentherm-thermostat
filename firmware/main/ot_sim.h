#pragma once
// Simulated OpenTherm boiler (slave) on top of plant_sim: answers frames like a Victrix Tera V2 with P7 = 2 is
// expected to, including the Immergas quirks the firmware has to handle, so they can be tested before the
// real boiler is installed. Pure C (no ESP-IDF).
#include <stdbool.h>
#include <stdint.h>
#include "plant_sim.h"

typedef struct {
    bool need_ca;          // writes are ACKed but ignored unless ID 0 carries low byte 0xCA
    bool heats_to_57;      // the boiler heats to ID 57 (max CH setpoint) instead of ID 1
    bool ignores_ch_off;   // clearing the CH bit alone does not stop the burner while ID 1 > 10 °C
    bool dhw_set_writable; // ID 56 writes take effect (false: ACKed, panel value kept, as with P7 = 2)
    float panel_max_ch;    // t1: the boiler never heats above this
    float e31_after_s;     // no valid frame for this long -> E31, CH locked out until a power cycle
    uint8_t miss_pct;      // % of requests left unanswered
} ot_sim_quirks_t;

typedef struct {
    plant_params_t plant_p;
    plant_state_t plant;
    ot_sim_quirks_t q;
    // OpenTherm-side state
    uint8_t master_flags;
    bool ca_seen;
    float id1, id57, dhw_set;
    bool id57_written;
    uint8_t fault_flags, oem_fault;
    bool e31, ch_locked;
    double last_frame_s;
    uint32_t rng;
    uint32_t frames, missed;
} ot_sim_t;

void ot_sim_default_quirks(ot_sim_quirks_t *q);
void ot_sim_init(ot_sim_t *b, float room_c, float tod_start_h);
// Answers one request. false = no answer (miss or E31 silence is not modelled; misses only).
bool ot_sim_respond(ot_sim_t *b, uint32_t req, uint32_t *resp);
// Advances the simulated plant and the comm-loss timer.
void ot_sim_tick(ot_sim_t *b, float dt_s);
// Boiler power cycle: clears E31; CH stays locked if no master is talking (manual: "local mode").
void ot_sim_power_cycle(ot_sim_t *b);
void ot_sim_set_fault(ot_sim_t *b, uint8_t oem_code);  // 0 clears
// What the boiler is actually heating to (after the quirks), and whether CH is on from its point of view.
float ot_sim_effective_target(const ot_sim_t *b, bool *ch_on);
