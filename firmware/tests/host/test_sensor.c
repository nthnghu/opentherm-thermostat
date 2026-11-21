// Room sensor filter (sensor_filter.c): REVIEW.md T1/T2 and the recovery steps of #39.
#include <math.h>
#include "test.h"
#include "sensor_filter.h"

#define STEP_US 2000000LL  // one read every 2 s
#define STALE_S 30

// Feeds n reads of the same result; returns the last action and counts soft/bus resets and stuck reports.
typedef struct {
    int soft, bus, stuck;
} counts_t;

static void feed(sensor_filter_t *f, int64_t *now, int n, bool read_ok, float t, float h, counts_t *c)
{
    for (int i = 0; i < n; i++) {
        sf_action_t a;
        bool stuck;
        bool ok = sf_check(f, read_ok, t, h, &a, &stuck);
        sf_accept(f, *now, ok, t, h);
        if (c) {
            c->soft += a == SF_ACT_SOFT_RESET;
            c->bus += a == SF_ACT_BUS_RESET;
            c->stuck += stuck;
        }
        *now += STEP_US;
    }
}

// Like a real sensor: the last digits move
static void feed_noisy(sensor_filter_t *f, int64_t *now, int n, float t, float h)
{
    for (int i = 0; i < n; i++) {
        float d = (i % 3 - 1) * 0.01f;
        sf_action_t a;
        bool stuck;
        bool ok = sf_check(f, true, t + d, h - d, &a, &stuck);
        sf_accept(f, *now, ok, t + d, h - d);
        *now += STEP_US;
    }
}

// T1: NaN, -45 °C (all-zero word), 130 °C (all-ones word), then valid again
static void test_t1_bad_values(void)
{
    sensor_filter_t f;
    sf_init(&f, STALE_S);
    int64_t now = 1000000;
    CHECK(!f.ok && f.age_s == -1, "no reading yet: not ok, age -1");
    feed_noisy(&f, &now, 60, 21.0f, 50);
    CHECK(f.ok && NEAR(f.room_c, 21.0, 0.02) && f.room_shown == 21.0f, "settled at %.2f", f.room_c);

    uint32_t e0 = f.errors;
    float before = f.room_c;
    // The driver returns these as valid readings (CRC fine): only the range test stops them
    feed(&f, &now, 1, true, NAN, 50, NULL);
    feed(&f, &now, 1, true, -45.0f, 50, NULL);
    feed(&f, &now, 1, true, 130.0f, 50, NULL);
    feed(&f, &now, 1, true, 21.0f, NAN, NULL);
    CHECK(f.errors == e0 + 4, "4 rejected readings counted (%u)", f.errors - e0);
    CHECK(f.room_c == before && isfinite(f.humidity), "average untouched (%.2f, humidity %.1f)", f.room_c, f.humidity);
    CHECK(f.ok, "still ok 8 s after the last good reading");
    CHECK(f.back_bad == 0, "no recovery reported while still failing");

    // Keep failing past the stale time
    feed(&f, &now, 12, true, 130.0f, 50, NULL);
    CHECK(!f.ok && f.age_s >= STALE_S, "not ok after the stale time (age %d s)", f.age_s);
    CHECK(f.room_shown == 21.0f, "shown value kept (the screen says the sensor is lost)");

    // First valid reading after the gap: the average restarts at it, no slow crawl from the old value
    feed(&f, &now, 1, true, 23.4f, 40, NULL);
    CHECK(f.ok && f.age_s == 0, "ok again at once");
    CHECK(f.room_c == 23.4f && f.room_shown == 23.4f && f.humidity == 40, "restarted at the new reading (%.2f)",
          f.room_c);
}

// Normal noise is smoothed (~30 s) and the dead band keeps the shown value still on a .x5 border
static void test_smoothing(void)
{
    sensor_filter_t f;
    sf_init(&f, STALE_S);
    int64_t now = 1;
    feed_noisy(&f, &now, 30, 20.0f, 50);
    feed_noisy(&f, &now, 15, 21.0f, 50);  // 30 s after a 1 K step
    CHECK(f.room_c > 20.5f && f.room_c < 20.75f, "30 s after a step: %.2f (about 1 - 1/e of the way)", f.room_c);
    feed_noisy(&f, &now, 200, 21.0f, 50);
    CHECK(NEAR(f.room_c, 21.0, 0.02), "settled %.3f", f.room_c);

    sf_init(&f, STALE_S);
    now = 1;
    int flips = 0;
    float shown = 0;
    for (int i = 0; i < 600; i++) {  // 20 minutes sitting on 21.05 with +-0.03 noise
        float t = 21.05f + ((i * 7) % 7 - 3) * 0.01f;
        sf_action_t a;
        bool stuck;
        sf_accept(&f, now, sf_check(&f, true, t, 50 + (i % 2) * 0.01f, &a, &stuck), t, 50);
        if (i > 0 && f.room_shown != shown) {
            flips++;
        }
        shown = f.room_shown;
        now += STEP_US;
    }
    CHECK(flips == 0, "shown value did not flap on the 0.05 border (%d changes)", flips);
}

// T2 (as decided in #39): bit-identical readings for 5 minutes = the sensor stopped measuring
static void test_t2_stuck(void)
{
    sensor_filter_t f;
    sf_init(&f, STALE_S);
    int64_t now = 1;
    counts_t c = {0};
    feed(&f, &now, SF_STUCK_N, true, 21.37f, 48.2f, &c);
    CHECK(f.ok && c.stuck == 0, "%d identical reads (5 min): still trusted", SF_STUCK_N);
    feed(&f, &now, 1, true, 21.37f, 48.2f, &c);
    CHECK(c.stuck == 1, "the next identical read reports stuck once");
    feed(&f, &now, 20, true, 21.37f, 48.2f, &c);
    CHECK(c.stuck == 1, "reported only once (%d)", c.stuck);
    CHECK(!f.ok, "stuck sensor counts as lost after the stale time (age %d s)", f.age_s);
    CHECK(c.soft == 0 && c.bus == 0, "stuck values are not driver failures: no reset from the failure counter");
    // Noise again: back at once
    feed_noisy(&f, &now, 1, 21.5f, 48);
    CHECK(f.ok && NEAR(f.room_c, 21.5, 0.02), "readings move again: ok (%.2f)", f.room_c);

    // Identical temperature with moving humidity is not stuck
    sf_init(&f, STALE_S);
    now = 1;
    for (int i = 0; i < 400; i++) {
        sf_action_t a;
        bool stuck;
        float h = 50 + (i % 2) * 0.02f;
        sf_accept(&f, now, sf_check(&f, true, 21.0f, h, &a, &stuck), 21.0f, h);
        now += STEP_US;
    }
    CHECK(f.ok, "steady temperature, humidity moving: trusted");
}

// #39 recovery: soft reset after 5 failed reads, I2C bus reset after 15 and every 15 (30 s) after that
static void test_recovery_steps(void)
{
    sensor_filter_t f;
    sf_init(&f, STALE_S);
    int64_t now = 1;
    feed_noisy(&f, &now, 10, 21.0f, 50);
    counts_t c = {0};
    feed(&f, &now, 4, false, 0, 0, &c);
    CHECK(c.soft == 0 && c.bus == 0, "4 failures: nothing yet");
    feed(&f, &now, 1, false, 0, 0, &c);
    CHECK(c.soft == 1 && c.bus == 0, "5th failure: soft reset");
    feed(&f, &now, 10, false, 0, 0, &c);
    CHECK(c.soft == 1 && c.bus == 1, "15th failure: bus reset");
    feed(&f, &now, 30, false, 0, 0, &c);
    CHECK(c.soft == 1 && c.bus == 3, "then every 15 failures (30 s): %d bus resets", c.bus);
    CHECK(!f.ok && f.errors == 45, "lost, 45 errors (%u)", f.errors);
    feed_noisy(&f, &now, 1, 22.0f, 50);
    CHECK(f.ok && f.fails_in_row == 0 && NEAR(f.room_c, 22.0, 0.02), "back: counter cleared, average restarted");
    CHECK(f.back_bad == 45 && f.back_s == 92, "recovery reported: %d failed reads, %d s", f.back_bad, f.back_s);
    feed_noisy(&f, &now, 1, 22.0f, 50);
    CHECK(f.back_bad == 0, "reported only on the first good reading");
    feed(&f, &now, 4, false, 0, 0, &c);
    CHECK(c.soft == 1, "a new run of failures starts counting from 0");
}

// Plugged in (or a loose contact settling) after the start: the first good reading is reported too
static void test_first_after_failures(void)
{
    sensor_filter_t f;
    sf_init(&f, STALE_S);
    int64_t now = 1;
    counts_t c = {0};
    feed(&f, &now, 20, false, 0, 0, &c);
    CHECK(!f.ok && f.age_s == -1 && f.back_bad == 0, "failing since the start: no reading yet");
    feed_noisy(&f, &now, 1, 19.0f, 55);
    CHECK(f.ok && f.back_bad == 20 && f.back_s == -1, "first reading after 20 failures reported (%d, %d s)",
          f.back_bad, f.back_s);
    feed_noisy(&f, &now, 1, 19.0f, 55);
    CHECK(f.back_bad == 0, "only once");
    sf_init(&f, STALE_S);
    feed_noisy(&f, &now, 1, 19.0f, 55);
    CHECK(f.back_bad == 0, "a sensor that works from the start reports nothing");
}

static void test_offset_shift(void)
{
    sensor_filter_t f;
    sf_init(&f, STALE_S);
    int64_t now = 1;
    feed_noisy(&f, &now, 100, 21.0f, 50);
    float c0 = f.room_c;
    sf_shift(&f, -0.5f);
    CHECK(NEAR(f.room_c, c0 - 0.5, 1e-4) && f.room_shown == 20.5f, "offset moves the average and the shown value");
    feed_noisy(&f, &now, 1, 20.5f, 50);
    CHECK(NEAR(f.room_c, 20.5, 0.02), "no jump on the next reading (%.2f)", f.room_c);
}

int main(void)
{
    test_t1_bad_values();
    test_smoothing();
    test_t2_stuck();
    test_recovery_steps();
    test_first_after_failures();
    test_offset_shift();
    TEST_DONE();
}
