// OpenTherm Manchester decoder (ot_manchester.h): REVIEW.md T12 on the line level, L10 (noise) as it behaves today.
#include <stdlib.h>
#include "test.h"
#include "ot_frame.h"
#include "ot_manchester.h"

#define MAX_EDGES 200

typedef struct {
    int64_t t[MAX_EDGES];
    int level[MAX_EDGES];
    int n;
} edges_t;

static void add_edge(edges_t *e, int64_t t, int level)
{
    if (e->n < MAX_EDGES) {
        e->t[e->n] = t;
        e->level[e->n] = level;
        e->n++;
    }
}

// The boiler's answer as edges on our input (1 = line active), half-bit length half_us, starting at t0
static void line_edges(uint32_t frame, int half_us, int64_t t0, edges_t *e)
{
    uint8_t lv[OTM_HALF_BITS];
    otm_encode(frame, lv, 1, 0);
    e->n = 0;
    int prev = 0;  // idle before the start bit
    for (int k = 0; k < OTM_HALF_BITS; k++) {
        if (lv[k] != prev) {
            add_edge(e, t0 + (int64_t)k * half_us, lv[k]);
            prev = lv[k];
        }
    }
}

// Feeds edges in time order; returns the state at the end (OTM_READY / OTM_INVALID, or still receiving)
static otm_state_t decode(const edges_t *e, uint32_t *frame, int64_t *start)
{
    otm_rx_t r;
    otm_rx_begin(&r);
    for (int i = 0; i < e->n; i++) {
        if (otm_rx_edge(&r, e->t[i], e->level[i])) {
            break;
        }
    }
    *frame = r.frame;
    if (start) {
        *start = r.start;
    }
    return r.st;
}

// Inserts a short pulse (two edges, inverting the line for width_us) at time t, keeping the order
static void insert_glitch(edges_t *e, int64_t t, int width_us)
{
    int i = 0;
    while (i < e->n && e->t[i] < t) {
        i++;
    }
    int cur = i ? e->level[i - 1] : 0;
    for (int k = e->n - 1; k >= i; k--) {
        e->t[k + 2] = e->t[k];
        e->level[k + 2] = e->level[k];
    }
    e->t[i] = t;
    e->level[i] = !cur;
    e->t[i + 1] = t + width_us;
    e->level[i + 1] = cur;
    e->n += 2;
}

static void test_encode(void)
{
    uint8_t lv[OTM_HALF_BITS];
    otm_encode(0x80000000, lv, 0, 1);  // the shield's output: 0 = active
    CHECK(lv[0] == 0 && lv[1] == 1, "start bit: active then idle");
    CHECK(lv[2] == 0 && lv[3] == 1, "first data bit 1: active then idle");
    CHECK(lv[4] == 1 && lv[5] == 0, "data bit 0: idle then active");
    CHECK(lv[66] == 0 && lv[67] == 1, "stop bit: active then idle, line left idle");
}

static void test_round_trip(void)
{
    uint32_t frames[] = {
        ot_frame(OT_READ_ACK, 0, 0x0300),  // status reply
        ot_frame(OT_READ_ACK, 25, 0x2C80), // flow 44.5 °C
        ot_frame(OT_UNKNOWN_DATA_ID, 99, 0),
        0x00000000,
        0xFFFFFFFF,
    };
    int ok = 0, n = sizeof(frames) / sizeof(frames[0]);
    for (int i = 0; i < n; i++) {
        edges_t e;
        line_edges(frames[i], 500, 1000, &e);
        uint32_t got;
        int64_t start;
        otm_state_t st = decode(&e, &got, &start);
        ok += st == OTM_READY && got == frames[i] && start == 1000;
    }
    CHECK(ok == n, "%d of %d frames decoded exactly, start time kept for the latency", ok, n);
}

// The OpenTherm spec allows 900..1150 us per bit; a boiler's clock is not ours
static void test_bit_timing(void)
{
    uint32_t f = ot_frame(OT_READ_ACK, 17, 0x4100);
    int halves[] = {450, 480, 520, 575};
    for (int i = 0; i < 4; i++) {
        edges_t e;
        line_edges(f, halves[i], 0, &e);
        uint32_t got;
        CHECK(decode(&e, &got, NULL) == OTM_READY && got == f, "bit time %d us decodes", 2 * halves[i]);
    }
}

static void test_bad_starts(void)
{
    edges_t e = {0};
    add_edge(&e, 100, 0);  // falling first: the line was not idle, or noise
    uint32_t got;
    CHECK(decode(&e, &got, NULL) == OTM_INVALID, "a first edge to idle is invalid");

    e.n = 0;
    add_edge(&e, 100, 1);
    add_edge(&e, 1100, 0);  // start bit too long: no mid-bit edge within 750 us
    CHECK(decode(&e, &got, NULL) == OTM_INVALID, "a start bit without its mid-bit edge is invalid");

    line_edges(ot_frame(OT_READ_ACK, 0, 0x0300), 500, 0, &e);
    e.n -= 3;  // the answer stops halfway through the last bits (line dropped)
    CHECK(decode(&e, &got, NULL) != OTM_READY, "a cut-off answer never becomes ready (the 874 ms timeout ends it)");
}

// L10 (left as it is): what short spikes do today
static void test_noise(void)
{
    uint32_t f = ot_frame(OT_READ_ACK, 25, 0x2C80);
    uint32_t got;
    edges_t e;
    // A spike in the first 750 us after a mid-bit edge is ignored
    line_edges(f, 500, 0, &e);
    insert_glitch(&e, (3 + 2 * 9) * 500 + 200, 20);  // 200 us after data bit 9's mid-bit edge
    CHECK(decode(&e, &got, NULL) == OTM_READY && got == f, "a 20 us spike inside the ignore window: no effect");

    // A spike late in a bit is taken as the mid-bit edge: the frame is corrupted. Parity catches it unless an even
    // number of bits flipped; then the reply checks (message type, data-id) in otb_core are the next guard.
    srand(7);
    int runs = 500, wrong_ready = 0, caught = 0;
    for (int i = 0; i < runs; i++) {
        uint32_t fr = ot_frame(OT_READ_ACK, (uint8_t)(rand() % 128), (uint16_t)rand());
        line_edges(fr, 500, 0, &e);
        int64_t t = 2000 + rand() % 30000;
        insert_glitch(&e, t, 10 + rand() % 40);
        otm_state_t st = decode(&e, &got, NULL);
        if (st != OTM_READY || got == fr) {
            continue;
        }
        if (ot_parity_ok(got) && ot_type(got) == ot_type(fr) && ot_id(got) == ot_id(fr)) {
            wrong_ready++;  // would be used as data
        } else {
            caught++;
        }
    }
    printf("   noise: %d of %d spiked frames corrupted and caught by parity/type/id, %d passed as wrong data\n",
           caught, runs, wrong_ready);
    CHECK(caught > 0, "random spikes produce corrupted frames that are caught");
    CHECK(wrong_ready * 20 < runs, "under 5 %% of single random spikes give wrong data that passes every check (%d)",
          wrong_ready);
}

int main(void)
{
    test_encode();
    test_round_trip();
    test_bit_timing();
    test_bad_starts();
    test_noise();
    TEST_DONE();
}
