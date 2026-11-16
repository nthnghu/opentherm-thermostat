#pragma once
// USB-serial-only development commands (screenshot).
#include <stdio.h>
void debug_start(void);
// Runs one serial command (STAT, CFG, SVC, OTSTAT, SIM, GO ...) and writes the answer to o. Not SNAP.
void debug_exec(const char *line, FILE *o);
// CPU use per task since the previous call (or since boot), busiest first, one line: "name core %, ...".
// Needs CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS; otherwise writes "n/a".
void debug_task_stats(char *out, size_t size);
void debug_heap_stats(char *out, size_t size);
