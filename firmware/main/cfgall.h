#pragma once
// All settings tables behind one index (control, schedule + hot water, alerts, service), for the touch
// editor, the web forms, JSON import/export and the serial CFG command. Changes are saved like any other
// settings change (thermostat.c, NVS, a few seconds later).
#include <stdbool.h>
#include <stddef.h>
#include "cfgtable.h"

typedef enum { CFG_T_CTRL = 0, CFG_T_SCHED, CFG_T_ALERT, CFG_T_SVC, CFG_T_COUNT } cfg_table_t;

int cfgall_count(void);
const ct_field_t *cfgall_field(int i, cfg_table_t *table);  // NULL past the end
const char *cfgall_table_title(cfg_table_t t);
int cfgall_find(const char *name);                           // -1 = unknown
bool cfgall_get(int i, char *out, size_t size);
// Range-checked; false on an unknown name or a bad value.
bool cfgall_set(const char *name, const char *value);
// Factory values for every table.
void cfgall_defaults(void);
