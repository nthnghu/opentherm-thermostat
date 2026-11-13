#pragma once
// Small web page for entering the Home Assistant URL + token from a phone.
// Served on the board's LAN IP (port 80) while the Setup screen is open.
#include <stdbool.h>
#include "esp_err.h"

esp_err_t portal_start(void);
void portal_stop(void);
bool portal_running(void);

// Tests HA access with the saved settings: GET <url>/api/ with the token.
// Writes a short human-readable result into msg; returns true on success.
bool portal_test_ha(char *msg, size_t msg_size);

// Result of the last test (from the web page or the Setup screen), "" if none yet.
const char *portal_last_result(void);
