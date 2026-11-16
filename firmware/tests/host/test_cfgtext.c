// Plain-language setting texts: complete, unique, consistent with the settings tables, fit the screen.
#include <string.h>
#include "test.h"
#include "cfgtext.h"
#include "control.h"
#include "schedule.h"
#include "alerts.h"
#include "service.h"

typedef const ct_field_t *(*field_fn)(int);
static const field_fn TABLES[] = {ctrl_config_field, sched_config_field, alert_config_field, svc_config_field};

static const ct_field_t *find_field(const char *name)
{
    for (size_t t = 0; t < sizeof(TABLES) / sizeof(TABLES[0]); t++) {
        for (int i = 0; TABLES[t](i); i++) {
            if (strcmp(TABLES[t](i)->name, name) == 0) {
                return TABLES[t](i);
            }
        }
    }
    return NULL;
}

// ASCII apart from the degree sign (the device fonts lack minus / plus-minus).
static bool font_safe(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p >= 0x80) {
            if (p[0] == 0xC2 && p[1] == 0xB0) {
                p++;
                continue;
            }
            return false;
        }
    }
    return true;
}

static void test_complete(void)
{
    int fields = 0;
    for (size_t t = 0; t < sizeof(TABLES) / sizeof(TABLES[0]); t++) {
        for (int i = 0; TABLES[t](i); i++) {
            const ct_field_t *f = TABLES[t](i);
            fields++;
            CHECK(cfgtext_get(f->name), "text for %s", f->name);
            CHECK(find_field(f->name) == f, "%s is unique across the tables", f->name);
        }
    }
    CHECK(fields == cfgtext_count(), "%d settings, %d texts", fields, cfgtext_count());
}

static void test_texts(void)
{
    int last_group = 0;
    for (int i = 0; i < cfgtext_count(); i++) {
        const cfgtext_t *t = cfgtext_at(i);
        const ct_field_t *f = find_field(t->name);
        CHECK(f, "%s is a setting", t->name);
        CHECK((int)t->group >= last_group, "%s: groups in display order", t->name);
        last_group = (int)t->group;
        CHECK(strlen(t->title) <= 28, "%s: title fits (%zu)", t->name, strlen(t->title));
        CHECK(strlen(t->brief) <= 52, "%s: brief fits (%zu)", t->name, strlen(t->brief));
        CHECK(strlen(t->help) <= 330, "%s: help fits (%zu)", t->name, strlen(t->help));
        CHECK(font_safe(t->title) && font_safe(t->brief) && font_safe(t->help), "%s: glyphs", t->name);
        CHECK(!strchr(t->help, '_') && !strchr(t->brief, '_') && !strchr(t->title, '_'), "%s: no internal names",
              t->name);
        if (t->opts) {
            CHECK(f && f->min == 0 && (int)f->max == t->nopts - 1, "%s: one choice per value", t->name);
            for (int k = 0; k < t->nopts; k++) {
                CHECK(font_safe(t->opts[k].label) && font_safe(t->opts[k].desc), "%s: choice glyphs", t->name);
            }
        }
        char pn[32];
        int pv;
        if (cfgtext_parent(t, pn, sizeof(pn), &pv)) {
            const ct_field_t *p = find_field(pn);
            CHECK(p && pv >= p->min && pv <= p->max, "%s: parent %s", t->name, t->parent);
        }
    }
}

static void test_format(void)
{
    char o[48];
    cfgtext_format(find_field("eco_setback_k"), "2.00", o, sizeof(o));
    CHECK(strcmp(o, "2 °C") == 0, "%s", o);
    cfgtext_format(find_field("dhw_mode"), "1", o, sizeof(o));
    CHECK(strcmp(o, "Time windows") == 0, "%s", o);
    cfgtext_format(find_field("prog_on"), "1", o, sizeof(o));
    CHECK(strcmp(o, "On") == 0, "%s", o);
    cfgtext_format(find_field("boost_min"), "90", o, sizeof(o));
    CHECK(strcmp(o, "1 h 30 min") == 0, "%s", o);
    cfgtext_format(find_field("duty_min_off_s"), "240", o, sizeof(o));
    CHECK(strcmp(o, "4 min") == 0, "%s", o);
    cfgtext_format(find_field("link_s"), "45", o, sizeof(o));
    CHECK(strcmp(o, "45 s") == 0, "%s", o);
    cfgtext_format(find_field("dhw_boost_min"), "0", o, sizeof(o));
    CHECK(strcmp(o, "0 (off)") == 0, "%s", o);
    cfgtext_format(find_field("dhw_win3_start"), "-", o, sizeof(o));
    CHECK(strcmp(o, "not used") == 0, "%s", o);
    cfgtext_format(find_field("loss_duty_pct"), "40", o, sizeof(o));
    CHECK(strcmp(o, "40%") == 0, "%s", o);
    cfgtext_format(find_field("pressure_min_bar"), "1.00", o, sizeof(o));
    CHECK(strcmp(o, "1 bar") == 0, "%s", o);
    cfgtext_range(find_field("boost_min"), o, sizeof(o));
    CHECK(strcmp(o, "10 min to 4 h") == 0, "%s", o);
    cfgtext_range(find_field("prog_on"), o, sizeof(o));
    CHECK(o[0] == 0, "no range for on/off");
    char pn[32];
    int pv;
    CHECK(cfgtext_parent(cfgtext_get("dhw_win1_start"), pn, sizeof(pn), &pv) && strcmp(pn, "dhw_mode") == 0 && pv == 1,
          "parent with a value");
    CHECK(cfgtext_parent(cfgtext_get("boost_k"), pn, sizeof(pn), &pv) && strcmp(pn, "boost_on") == 0 && pv == 1,
          "on/off parent");
    CHECK(!cfgtext_parent(cfgtext_get("prog_on"), pn, sizeof(pn), &pv), "no parent");
}

int main(void)
{
    test_complete();
    test_texts();
    test_format();
    TEST_DONE();
}
