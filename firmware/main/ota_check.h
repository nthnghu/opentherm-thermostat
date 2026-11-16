#pragma once
// A new firmware (after an OTA update) starts "on trial". It is kept only when
//   1. the hardware check passes (selftest), and
//   2. within OTA_TRIAL_S, Wi-Fi is connected and the web server works: it has handled a request since the
//      restart (tools/ota.sh and the web page poll it), or a TCP connect to its port over loopback succeeds,
// so an update can never lock the device out of further updates over the network. Otherwise the device rolls
// back to the previous firmware and restarts. A restart during the trial (e.g. the Wi-Fi watchdog) also rolls back:
// the bootloader does not start an image twice on trial.
#include <stdbool.h>

#define OTA_TRIAL_S 300

typedef enum { OTA_ST_VALID = 0, OTA_ST_TRIAL, OTA_ST_FACTORY } ota_st_t;

// Call once after the hardware check and after webui_start().
void ota_check_start(bool hw_ok);
ota_st_t ota_check_state(void);
const char *ota_check_text(void);  // e.g. "accepted", "on trial: waiting for Wi-Fi (240 s left)"
