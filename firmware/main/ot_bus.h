#pragma once
// OpenTherm bus task: the only user of the PHY. Runs otb_core every second on core 1, against the real boiler
// (DIYLESS shield) or the simulated Immergas boiler (ot_sim + plant_sim). The cadence never stops.
// Every frame is kept in a PSRAM ring (frame capture) for /api/otlog.csv.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "otb_core.h"

void otbus_start(void);
// Flash writers: wait until no OpenTherm frame is on the wire and hold the bus off until otbus_quiet_end().
// Returns false on timeout (write anyway or skip).
bool otbus_quiet_begin(uint32_t timeout_ms);
// Line test only: the bus task keeps looping (watchdog fed) but sends no frames while paused.
void otbus_pause(bool on);
void otbus_quiet_end(void);

void otbus_set_cmd(const otb_cmd_t *cmd);
void otbus_get(otb_status_t *st);
void otbus_get_mode(otb_status_t *st, bool *sim);  // the status and whether it came from the simulated boiler
void otbus_get_support(uint8_t out[128]);  // otb_support_t per ID
void otbus_get_flags(otb_flags_t *f);
// save = also keep them in NVS (otherwise RAM only, as the service routines use them).
void otbus_set_flags(const otb_flags_t *f, bool save);

// Service: one-off frame (false while another is pending) and its state; silence on the line.
bool otbus_manual(uint8_t type, uint8_t id, uint16_t data);
void otbus_manual_get(otb_manual_t *m);
void otbus_set_silent(bool silent);

void otbus_set_simulated(bool sim);
bool otbus_simulated(void);
// Room temperature of the simulated house (only while simulating). false = not simulating.
bool otbus_sim_room(float *room_c);
// Text commands for the simulator and the flags (serial "SIM ..."). Writes a reply into out.
void otbus_command(const char *cmd, char *out, size_t size);

typedef struct {
    uint32_t t_ms;   // since boot
    uint32_t req, resp;
    uint16_t latency_ms;
    uint8_t result;  // otb_xfer_result_t
    uint8_t sim;
} otbus_frame_t;

#define OTBUS_FRAME_RING 32768  // ~3 h at 3 frames/s (512 KB PSRAM)

// Copies up to max of the most recent frames, oldest first; returns the count.
int otbus_frames(otbus_frame_t *out, int max);
