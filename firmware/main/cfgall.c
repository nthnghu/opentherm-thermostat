#include "cfgall.h"
#include <string.h>
#include "thermostat.h"

typedef struct {
    int (*count)(void);
    const ct_field_t *(*field)(int);
    const char *title;
} table_t;

static const table_t T[CFG_T_COUNT] = {
    {ctrl_config_count, ctrl_config_field, "Heating control"},
    {sched_config_count, sched_config_field, "Schedule and hot water"},
    {alert_config_count, alert_config_field, "Alerts"},
    {svc_config_count, svc_config_field, "Service"},
};

int cfgall_count(void)
{
    int n = 0;
    for (int t = 0; t < CFG_T_COUNT; t++) {
        n += T[t].count();
    }
    return n;
}

static bool locate(int i, cfg_table_t *table, int *idx)
{
    for (int t = 0; t < CFG_T_COUNT && i >= 0; t++) {
        int n = T[t].count();
        if (i < n) {
            *table = (cfg_table_t)t;
            *idx = i;
            return true;
        }
        i -= n;
    }
    return false;
}

const ct_field_t *cfgall_field(int i, cfg_table_t *table)
{
    cfg_table_t t;
    int k;
    if (!locate(i, &t, &k)) {
        return NULL;
    }
    if (table) {
        *table = t;
    }
    return T[t].field(k);
}

const char *cfgall_table_title(cfg_table_t t)
{
    return t < CFG_T_COUNT ? T[t].title : "?";
}

int cfgall_find(const char *name)
{
    int n = cfgall_count();
    for (int i = 0; i < n; i++) {
        if (strcmp(cfgall_field(i, NULL)->name, name) == 0) {
            return i;
        }
    }
    return -1;
}

bool cfgall_get(int i, char *out, size_t size)
{
    cfg_table_t t;
    int k;
    if (!locate(i, &t, &k)) {
        return false;
    }
    switch (t) {
    case CFG_T_CTRL: {
        ctrl_config_t c;
        thermo_get_ctrl_config(&c);
        return ctrl_config_get(&c, k, out, size);
    }
    case CFG_T_SCHED: {
        sched_config_t c;
        thermo_get_sched_config(&c);
        return sched_config_get(&c, k, out, size);
    }
    case CFG_T_ALERT: {
        alert_config_t c;
        thermo_get_alert_config(&c);
        return alert_config_get(&c, k, out, size);
    }
    default: {
        svc_config_t c;
        thermo_get_svc_config(&c);
        return svc_config_get(&c, k, out, size);
    }
    }
}

bool cfgall_set(const char *name, const char *value)
{
    int i = cfgall_find(name);
    cfg_table_t t;
    int k;
    if (i < 0 || !locate(i, &t, &k)) {
        return false;
    }
    switch (t) {
    case CFG_T_CTRL: {
        ctrl_config_t c;
        thermo_get_ctrl_config(&c);
        if (!ctrl_config_set(&c, name, value)) {
            return false;
        }
        thermo_set_ctrl_config(&c);
        return true;
    }
    case CFG_T_SCHED: {
        sched_config_t c;
        thermo_get_sched_config(&c);
        if (!sched_config_set(&c, name, value)) {
            return false;
        }
        thermo_set_sched_config(&c);
        return true;
    }
    case CFG_T_ALERT: {
        alert_config_t c;
        thermo_get_alert_config(&c);
        if (!alert_config_set(&c, name, value)) {
            return false;
        }
        thermo_set_alert_config(&c);
        return true;
    }
    default: {
        svc_config_t c;
        thermo_get_svc_config(&c);
        if (!svc_config_set(&c, name, value)) {
            return false;
        }
        thermo_set_svc_config(&c);
        return true;
    }
    }
}

void cfgall_defaults(void)
{
    ctrl_config_t c;
    ctrl_default_config(&c);
    thermo_set_ctrl_config(&c);
    sched_config_t s;
    thermo_get_sched_config(&s);
    sched_config_t d;
    sched_default_config(&d);
    memcpy(d.prog, s.prog, sizeof(d.prog));  // keep the weekly program
    thermo_set_sched_config(&d);
    alert_config_t a;
    alert_default_config(&a);
    thermo_set_alert_config(&a);
    svc_config_t v;
    svc_default_config(&v);
    thermo_set_svc_config(&v);
}
