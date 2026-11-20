// Away mode in two NVS keys (awaykeys.h): REVIEW.md T17, a power cut between the two writes of save().
#include "test.h"
#include "awaykeys.h"

#define FRI 1767225600LL  // 2026-01-01, plausible end times
#define MON 1767484800LL

typedef struct {
    bool on;
    int64_t until;  // 0 = open-ended (only meaningful when on)
} away_t;

typedef struct {  // the two NVS keys as they are in flash
    bool have_flag, have_until;
    uint8_t flag;
    int64_t until;
} nvs_t;

static void write_key(nvs_t *n, away_key_t k, away_t s)
{
    if (k == AWAY_KEY_FLAG) {
        n->have_flag = true;
        n->flag = s.on;
    } else {
        n->have_until = true;
        n->until = s.on ? s.until : 0;
    }
}

// save() of state s, cut after `writes` key writes (0, 1 or 2)
static void save_cut(nvs_t *n, away_t s, int writes, const away_key_t order[2])
{
    for (int i = 0; i < writes; i++) {
        write_key(n, order[i], s);
    }
}

static away_t load(const nvs_t *n)
{
    away_t s;
    s.on = away_load(n->have_flag, n->flag, n->have_until, n->until, &s.until);
    return s;
}

static bool same(away_t a, away_t b)
{
    return a.on == b.on && (!a.on || a.until == b.until);
}

// Every change between these states, cut after 0, 1 and 2 writes: what loads is the old or the new state
static int mixes(bool old_order)
{
    away_t st[] = {{false, 0}, {true, 0}, {true, FRI}, {true, MON}};
    int bad = 0;
    for (int a = 0; a < 4; a++) {
        for (int b = 0; b < 4; b++) {
            for (int w = 0; w <= 2; w++) {
                nvs_t n = {0};
                away_key_t order[2];
                away_save_order(st[a].on, order);
                save_cut(&n, st[a], 2, order);  // old state fully stored
                if (old_order) {
                    order[0] = AWAY_KEY_UNTIL;
                    order[1] = AWAY_KEY_FLAG;
                } else {
                    away_save_order(st[b].on, order);
                }
                save_cut(&n, st[b], w, order);
                away_t got = load(&n);
                if (!same(got, st[a]) && !same(got, st[b])) {
                    bad++;
                    if (!old_order) {
                        printf("   %d -> %d cut after %d: loaded on=%d until=%lld\n", a, b, w, got.on,
                               (long long)got.until);
                    }
                }
            }
        }
    }
    return bad;
}

int main(void)
{
    CHECK(mixes(true) > 0, "the old order (end time, then flag) does mix states: the test can see the bug");
    CHECK(mixes(false) == 0, "every change, cut anywhere: the old or the new state loads");

    // The case from the review: away until Friday, switched off, cut after the first write
    nvs_t n = {0};
    away_key_t order[2];
    away_t fri = {true, FRI}, off = {false, 0};
    away_save_order(true, order);
    save_cut(&n, fri, 2, order);
    away_save_order(false, order);
    save_cut(&n, off, 1, order);
    away_t got = load(&n);
    CHECK(!got.on, "away until Friday -> off, cut halfway: not away for good (on=%d until=%lld)", got.on,
          (long long)got.until);

    // Load checks
    int64_t u;
    CHECK(!away_load(false, 0, false, 0, &u), "nothing stored: not away");
    CHECK(!away_load(true, 1, false, 0, &u), "flag without an end time key: not away");
    CHECK(away_load(true, 1, true, 0, &u) && u == 0, "flag with end 0: away, open-ended");
    CHECK(away_load(true, 1, true, FRI, &u) && u == FRI, "flag with a plausible end: away until then");
    CHECK(!away_load(true, 1, true, 12345, &u), "damaged end time: not away");
    CHECK(!away_load(true, 0, true, FRI, &u), "flag off: not away whatever the end says");
    TEST_DONE();
}
