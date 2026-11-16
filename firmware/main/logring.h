#pragma once
// Keeps the last LOGRING_SIZE bytes of the ESP log in PSRAM (the serial console still gets everything), so the
// log can be read without USB: web /api/log.
#include <stddef.h>

#define LOGRING_SIZE (64 * 1024)

void logring_start(void);
// Copies the log, oldest first, into out (size bytes, NUL-terminated). Returns the length.
size_t logring_read(char *out, size_t size);
