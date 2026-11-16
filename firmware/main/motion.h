#pragma once
// Camera-based presence detection that wakes the screen (Setup > Display > Wake on motion).
#include <stdbool.h>
#include <stdint.h>

typedef enum { MOTION_OFF, MOTION_WATCHING, MOTION_TOO_DARK, MOTION_PAUSED, MOTION_NO_CAMERA } motion_state_t;

void motion_start(void);
void motion_set_off(bool off);  // camera off until switched on again or restart (not saved)
void motion_set_paused(bool paused);  // while the camera is used for the QR scan
motion_state_t motion_state(void);
const char *motion_state_name(void);
int motion_seconds_since(void);       // -1 = no motion seen yet
uint32_t motion_count(void);
typedef struct {
    int mean, changed, contrast, delta_x100, frames, grab_fail;
} motion_debug_t;
void motion_debug(motion_debug_t *d);  // USB debug STAT
