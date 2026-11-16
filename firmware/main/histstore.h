#pragma once
// Keeps the data log (datalog.c) in a small flash partition "hist" (256 KB), so a restart (update, Wi-Fi
// watchdog) keeps the last 24 h of minute records and burns and 35 days of hourly summaries.
// Three rings of 4 KB sectors with fixed-size slots {magic, crc16, seq, payload}; a sector is erased just
// before the ring writes into it. Each minute adds one 64-byte slot (a sector is erased about once a day).
#include <stdbool.h>
#include "datalog.h"

bool histstore_init(void);                // false: no "hist" partition (flash the partition table over USB)
bool histstore_ok(void);
// Puts the saved entries back into d (call once, before the first datalog_tick).
void histstore_restore(dl_t *d);
// Writes the entries d gained since the last call. Call from the task that feeds d, after datalog_tick.
void histstore_sync(const dl_t *d);
// The newest saved minute record from before this start (for the PI correction); false = none.
bool histstore_last_rec(dl_rec_t *out);
void histstore_status(char *out, size_t size);  // e.g. "1440 minutes, 37 burns, 120 hours"
