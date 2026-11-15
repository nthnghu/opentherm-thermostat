#include "cfgtable.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static float get_f(const void *obj, const ct_field_t *f)
{
    const uint8_t *p = (const uint8_t *)obj + f->off;
    switch (f->type) {
    case CT_BOOL: return *(const bool *)p;
    case CT_U8: return *p;
    case CT_U16:
    case CT_TIME:
    case CT_TIME_OPT: return *(const uint16_t *)p;
    default: return *(const float *)p;
    }
}

static void put_f(void *obj, const ct_field_t *f, float v)
{
    uint8_t *p = (uint8_t *)obj + f->off;
    if (f->type == CT_TIME_OPT && v == CT_UNUSED) {
        *(uint16_t *)p = CT_UNUSED;
        return;
    }
    if (isnan(v)) {
        v = f->min;
    }
    v = fminf(fmaxf(v, f->min), f->max);
    switch (f->type) {
    case CT_BOOL: *(bool *)p = v != 0; break;
    case CT_U8: *p = (uint8_t)lroundf(v); break;
    case CT_U16:
    case CT_TIME:
    case CT_TIME_OPT: *(uint16_t *)p = (uint16_t)lroundf(v); break;
    default: *(float *)p = v; break;
    }
}

void ct_sanitize(void *obj, const ct_field_t *f, int n)
{
    for (int i = 0; i < n; i++) {
        if (f[i].type == CT_BOOL) {
            uint8_t *p = (uint8_t *)obj + f[i].off;  // any byte value -> 0/1 (garbage NVS blobs)
            *p = *p != 0;
            continue;
        }
        put_f(obj, &f[i], get_f(obj, &f[i]));
    }
}

void ct_get(const void *obj, const ct_field_t *f, char *out, size_t size)
{
    float v = get_f(obj, f);
    if (f->type == CT_TIME || f->type == CT_TIME_OPT) {
        uint16_t m = (uint16_t)v;
        if (m == CT_UNUSED) {
            snprintf(out, size, "-");
        } else {
            snprintf(out, size, "%02d:%02d", m / 60, m % 60);
        }
    } else if (f->type == CT_FLOAT) {
        snprintf(out, size, "%.2f", v);
    } else {
        snprintf(out, size, "%d", (int)v);
    }
}

bool ct_is_text(const ct_field_t *f, const char *formatted)
{
    return f->type == CT_TIME || f->type == CT_TIME_OPT;
}

// "H:MM"/"HH:MM" -> minutes, "-" -> CT_UNUSED, -1 = error
static int parse_time(const char *s)
{
    if (strcmp(s, "-") == 0) {
        return CT_UNUSED;
    }
    int h, m;
    char tail;
    if (sscanf(s, "%d:%d%c", &h, &m, &tail) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
        return -1;
    }
    return h * 60 + m;
}

bool ct_set(void *obj, const ct_field_t *fields, int n, const char *name, const char *value)
{
    for (int i = 0; i < n; i++) {
        const ct_field_t *f = &fields[i];
        if (strcmp(f->name, name) != 0) {
            continue;
        }
        float v;
        if (f->type == CT_TIME || f->type == CT_TIME_OPT) {
            int m = parse_time(value);
            if (m < 0 || (m == CT_UNUSED && f->type != CT_TIME_OPT)) {
                return false;
            }
            v = (float)m;
        } else if (f->type == CT_BOOL && (strcasecmp(value, "on") == 0 || strcasecmp(value, "true") == 0)) {
            v = 1;
        } else if (f->type == CT_BOOL && (strcasecmp(value, "off") == 0 || strcasecmp(value, "false") == 0)) {
            v = 0;
        } else {
            char *end;
            v = strtof(value, &end);
            if (end == value || *end || isnan(v)) {
                return false;
            }
            if (v < f->min || v > f->max) {
                return false;
            }
        }
        put_f(obj, f, v);
        return true;
    }
    return false;
}

void ct_step_value(const ct_field_t *f, const char *cur, int dir, char *out, size_t size)
{
    int sgn = dir > 0 ? 1 : -1;
    if (f->type == CT_BOOL) {
        snprintf(out, size, "%d", strcmp(cur, "0") == 0 ? 1 : 0);
        return;
    }
    if (f->type == CT_TIME || f->type == CT_TIME_OPT) {
        int m = parse_time(cur);
        if (m == CT_UNUSED) {
            m = sgn > 0 ? 0 : (f->type == CT_TIME_OPT ? CT_UNUSED : 0);
        } else {
            m = (m / 15) * 15 + sgn * 15;
            if (m < 0) {
                m = f->type == CT_TIME_OPT ? CT_UNUSED : 0;
            } else if (m > 1439) {
                m = 1425;
            }
        }
        if (m == CT_UNUSED) {
            snprintf(out, size, "-");
        } else {
            snprintf(out, size, "%02d:%02d", m / 60, m % 60);
        }
        return;
    }
    float v = strtof(cur, NULL), range = f->max - f->min, step;
    if (f->type == CT_FLOAT) {
        step = range <= 2 ? 0.05f : (range <= 15 ? 0.1f : (range <= 60 ? 0.5f : 1.0f));
    } else {
        step = range > 1000 ? 60 : (range > 300 ? 10 : 1);
    }
    v = roundf(v / step) * step + sgn * step;
    v = fminf(fmaxf(v, f->min), f->max);
    if (f->type == CT_FLOAT) {
        snprintf(out, size, "%.2f", v);
    } else {
        snprintf(out, size, "%d", (int)lroundf(v));
    }
}
