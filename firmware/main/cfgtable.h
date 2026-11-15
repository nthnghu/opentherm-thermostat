#pragma once
// Named, range-checked settings tables (serial CFG, web JSON). Pure C, host-tested.
// A table describes fields of a struct by offset; control.c, schedule.c and alerts.c each have one.
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    CT_BOOL,      // bool; "1/0", "on/off", "true/false"
    CT_U8,
    CT_U16,
    CT_FLOAT,
    CT_TIME,      // uint16 minute of the day, "HH:MM"
    CT_TIME_OPT,  // like CT_TIME, "-" = unused (0xFFFF)
} ct_type_t;

#define CT_UNUSED 0xFFFF

typedef struct {
    const char *name;
    ct_type_t type;
    size_t off;
    float min, max;
    const char *help;
} ct_field_t;

#define CT_FIELD(type_, member, t, lo, hi, h) {#member, t, offsetof(type_, member), lo, hi, h}

// Clamps every field to its range.
void ct_sanitize(void *obj, const ct_field_t *f, int n);
// Formats field f as text ("%.2f", integer, "HH:MM" or "-").
void ct_get(const void *obj, const ct_field_t *f, char *out, size_t size);
// Parses and range-checks a value for the named field. Returns false (obj unchanged) on a bad name or value.
bool ct_set(void *obj, const ct_field_t *f, int n, const char *name, const char *value);
// True when ct_get gives text that needs quotes in JSON.
bool ct_is_text(const ct_field_t *f, const char *formatted);
