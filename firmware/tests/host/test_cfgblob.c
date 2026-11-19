// Saved settings blobs (REVIEW.md T6). thermostat.c loads a blob only when its length and version match, else the
// defaults; whatever is loaded then goes through the *_sanitize function. These tests feed the sanitizers garbage
// blobs (all 0xFF, all 0x00, random bytes) and check that every setting ends inside its range.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "cfgtable.h"
#include "control.h"
#include "schedule.h"
#include "alerts.h"
#include "service.h"

static float field_value(const void *obj, const ct_field_t *f)
{
    const uint8_t *p = (const uint8_t *)obj + f->off;
    switch (f->type) {
    case CT_BOOL: return *p;
    case CT_U8: return *p;
    case CT_U16:
    case CT_TIME:
    case CT_TIME_OPT: {
        uint16_t v;
        memcpy(&v, p, 2);
        return v;
    }
    default: {
        float v;
        memcpy(&v, p, 4);
        return v;
    }
    }
}

// Every field of obj inside its range (bool 0/1, unused times allowed); returns the number of bad fields
static int bad_fields(const void *obj, int n, const ct_field_t *(*field)(int), const char *what, const char *fill)
{
    int bad = 0;
    for (int i = 0; i < n; i++) {
        const ct_field_t *f = field(i);
        float v = field_value(obj, f);
        bool ok = f->type == CT_BOOL ? (v == 0 || v == 1)
                  : (f->type == CT_TIME_OPT && v == CT_UNUSED) ? true
                  : isfinite(v) && v >= f->min && v <= f->max;
        if (!ok) {
            printf("   %s (%s): %s = %g outside %g..%g\n", what, fill, f->name, v, f->min, f->max);
            bad++;
        }
    }
    return bad;
}

typedef enum { FILL_FF, FILL_00, FILL_RANDOM } fill_t;
static const char *FILL_NAME[] = {"all 0xFF", "all 0x00", "random"};

static void fill(void *p, size_t n, fill_t how)
{
    uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        b[i] = how == FILL_FF ? 0xFF : how == FILL_00 ? 0 : (uint8_t)rand();
    }
}

#define CHECK_SANITIZE(type_, sanitize, count, field, ver_member, VERSION)                                     \
    do {                                                                                                         \
        int bad = 0, unstable = 0;                                                                               \
        for (int k = 0; k < 3 + 50; k++) {                                                                       \
            fill_t how = k < 3 ? (fill_t)k : FILL_RANDOM;                                                        \
            type_ a;                                                                                             \
            fill(&a, sizeof(a), how);                                                                            \
            a.ver_member = VERSION; /* a blob that passed the version check */                                   \
            sanitize(&a);                                                                                        \
            bad += bad_fields(&a, count(), field, #type_, FILL_NAME[how]);                                       \
            type_ b = a;                                                                                         \
            sanitize(&b);                                                                                        \
            unstable += memcmp(&a, &b, sizeof(a)) != 0;                                                          \
        }                                                                                                        \
        CHECK(bad == 0, #type_ ": every field in range after garbage (%d bad)", bad);                            \
        CHECK(unstable == 0, #type_ ": sanitizing twice changes nothing (%d)", unstable);                        \
    } while (0)

static void test_t6_garbage(void)
{
    srand(6);
    CHECK_SANITIZE(ctrl_config_t, ctrl_sanitize, ctrl_config_count, ctrl_config_field, version, CTRL_CONFIG_VERSION);
    CHECK_SANITIZE(sched_config_t, sched_sanitize, sched_config_count, sched_config_field, version,
                   SCHED_CONFIG_VERSION);
    CHECK_SANITIZE(alert_config_t, alert_sanitize, alert_config_count, alert_config_field, version,
                   ALERT_CONFIG_VERSION);
    CHECK_SANITIZE(svc_config_t, svc_sanitize, svc_config_count, svc_config_field, version, SVC_CONFIG_VERSION);
}

// The cross-field rules and the weekly program after garbage
static void test_t6_rules(void)
{
    srand(7);
    int bad_flow = 0, bad_prog = 0;
    for (int k = 0; k < 53; k++) {
        fill_t how = k < 3 ? (fill_t)k : FILL_RANDOM;
        ctrl_config_t c;
        fill(&c, sizeof(c), how);
        ctrl_sanitize(&c);
        bad_flow += c.flow_min_c > c.flow_max_c;

        sched_config_t s;
        fill(&s, sizeof(s), how);
        sched_sanitize(&s);
        for (int d = 0; d < 7; d++) {
            int last = -1;
            for (int i = 0; i < SCHED_POINTS; i++) {
                const sched_point_t *p = &s.prog[d][i];
                if (p->min == SCHED_UNUSED) {
                    last = 100000;  // only unused points after the first unused one
                    continue;
                }
                bool level_ok = p->level == SCHED_ECO || p->level == SCHED_COMFORT || sched_level_is_fixed(p->level);
                if (p->min > 1439 || p->min < last || last == 100000 || !level_ok) {
                    bad_prog++;
                }
                last = p->min;
            }
        }
        // Every day still answers with a valid level at any minute
        for (int d = 0; d < 7; d++) {
            float t = sched_level_temp(sched_level_at(&s, d, 720), 21, 3);
            bad_prog += !(t >= 5 && t <= 30);
        }
    }
    CHECK(bad_flow == 0, "flow_min never above flow_max after garbage");
    CHECK(bad_prog == 0, "program after garbage: sorted, unused points last, valid levels (%d bad)", bad_prog);
}

int main(void)
{
    test_t6_garbage();
    test_t6_rules();
    TEST_DONE();
}
