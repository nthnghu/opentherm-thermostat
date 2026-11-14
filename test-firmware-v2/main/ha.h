#pragma once
// Home Assistant REST client (read-only): GET <url><path> with the long-lived token.
#include <stdbool.h>
#include <stddef.h>

typedef enum { HA_STATE_UNKNOWN, HA_STATE_OK, HA_STATE_ERROR } ha_state_t;

// Returns a malloc'd (PSRAM) NUL-terminated body or NULL; *status = HTTP status or -1 (no connection).
char *ha_get(const char *path, int *status);
// GET /api/ check. Writes a short result; true when HA accepted the token.
bool ha_test(char *msg, size_t size);
const char *ha_last_result(void);
ha_state_t ha_state(void);
// True when the text looks like a long-lived access token (a JWT: three base64url parts).
bool ha_token_plausible(const char *s);
// Fetches sun.sun from HA (call from a worker task). Keeps the result for ha_sun_is_up().
void ha_sun_refresh(void);
// True and *up set when a sun.sun result younger than 30 min is known.
bool ha_sun_is_up(bool *up);
// Non-secret summary of a token for diagnostics: length and the issue time from its JWT payload.
void ha_token_describe(const char *token, char *out, size_t size);
