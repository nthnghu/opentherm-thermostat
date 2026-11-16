#pragma once
// USB-serial-only development commands (screenshot).
#include <stdio.h>
void debug_start(void);
// Runs one serial command (STAT, CFG, SVC, OTSTAT, SIM, GO ...) and writes the answer to o. Not SNAP.
void debug_exec(const char *line, FILE *o);
