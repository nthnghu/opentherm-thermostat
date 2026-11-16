#pragma once
// Plain-language texts for every setting: what the touch screen and the web page show instead of the internal
// names (which stay for the serial CFG command and the JSON export). Pure C, host-tested: every setting in the
// control / schedule / alert / service tables must have an entry here (tests/host/test_cfgtext.c).
#include <stdbool.h>
#include <stddef.h>
#include "cfgtable.h"

typedef enum {
    CG_PROGRAM = 0,  // weekly program, Eco, boost, away
    CG_SUMMER,
    CG_DHW,
    CG_LEGIONELLA,
    CG_SAFETY,       // frost, room sensor lost
    CG_SYSTEM,       // flow limits, underfloor
    CG_TUNING,       // control strategy and its constants (expert)
    CG_ALERTS,
    CG_SERVICE,      // commissioning test parameters (expert)
    CG_COUNT
} cfg_group_t;

typedef struct {
    const char *label;  // shown as the value
    const char *desc;   // what this choice does
} cfgtext_opt_t;

typedef struct {
    const char *name;     // internal name (cfgall / CFG / JSON)
    cfg_group_t group;
    const char *title;    // short, e.g. "Eco temperature drop"
    const char *brief;    // one line under the title in lists
    const char *help;     // full explanation in the editor
    const char *unit;     // "°C", "bar", "%", "h", "s" / "min" (shown as a duration), "" = none
    const char *parent;   // the setting this one belongs to: "boost_on" (in use while on) or "dhw_mode=1"
                          // (in use while that choice is selected); NULL = always in use
    const cfgtext_opt_t *opts;  // named choices for 0..nopts-1 (NULL = a number)
    int nopts;
} cfgtext_t;

const cfgtext_t *cfgtext_get(const char *name);  // NULL = no text (should not happen)
int cfgtext_count(void);
const cfgtext_t *cfgtext_at(int i);              // display order: by group, then as listed
const char *cfgtext_group_title(cfg_group_t g);
const char *cfgtext_group_brief(cfg_group_t g);
// Splits t->parent into the setting name and the value at which t is in use (1 for an on/off parent).
bool cfgtext_parent(const cfgtext_t *t, char *name, size_t size, int *value);

// The value as shown to people: "2 °C", "On", "Always", "1 h 30 min", "06:00", "not used".
// raw is the text from ct_get.
void cfgtext_format(const ct_field_t *f, const char *raw, char *out, size_t size);
// Range as shown to people, e.g. "0.5 to 6 °C", "10 min to 4 h". Empty for on/off and choices.
void cfgtext_range(const ct_field_t *f, char *out, size_t size);
