// Flash history ring (histring.c): REVIEW.md T10, power loss mid-slot and mid-erase, ring wrap, the newest record.
// The flash is simulated like NOR flash: erase sets a sector to 0xFF, a write can only clear bits.
#include <string.h>
#include "test.h"
#include "histring.h"

#define SECTORS 3
#define SLOT    64
#define PAYLOAD 40
#define NSLOTS  (SECTORS * HR_SECTOR / SLOT)  // 192
#define PER_SEC (HR_SECTOR / SLOT)            // 64

static uint8_t g_flash[SECTORS * HR_SECTOR];

typedef struct {
    uint32_t seq, next;
} writer_t;

static void flash_erase_all(void)
{
    memset(g_flash, 0xFF, sizeof(g_flash));
}

static void flash_write(uint32_t addr, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        g_flash[addr + i] &= p[i];
    }
}

static void payload_for(uint32_t v, uint8_t *p)
{
    for (int i = 0; i < PAYLOAD; i++) {
        p[i] = (uint8_t)(v * 7 + i);
    }
    memcpy(p, &v, sizeof(v));
}

// Like histstore.c write_slot: erase on a sector start, then write. bytes < SLOT simulates a power cut mid-write.
static void ring_write(writer_t *w, uint32_t v, size_t bytes)
{
    uint8_t slot[SLOT], p[PAYLOAD];
    payload_for(v, p);
    hr_make_slot(slot, SLOT, w->seq + 1, p, PAYLOAD);
    uint32_t a = w->next * SLOT;
    if (a % HR_SECTOR == 0) {
        memset(g_flash + a, 0xFF, HR_SECTOR);
    }
    flash_write(a, slot, bytes);
    if (bytes == SLOT) {
        w->seq++;
        w->next = (w->next + 1) % NSLOTS;
    }
}

typedef struct {
    uint32_t vals[NSLOTS + 1];
    int n;
    int bad_payload;
} got_t;

static void put(void *ctx, const void *payload)
{
    got_t *g = ctx;
    uint32_t v;
    memcpy(&v, payload, sizeof(v));
    uint8_t want[PAYLOAD];
    payload_for(v, want);
    g->bad_payload += memcmp(payload, want, PAYLOAD) != 0;
    if (g->n <= NSLOTS) {
        g->vals[g->n++] = v;
    }
}

static hr_scan_t scan(got_t *g)
{
    memset(g, 0, sizeof(*g));
    return hr_scan(g_flash, NSLOTS, SLOT, PAYLOAD, put, g);
}

// Restored values are consecutive and end at `last`
static bool consecutive_to(const got_t *g, uint32_t last)
{
    for (int i = 1; i < g->n; i++) {
        if (g->vals[i] != g->vals[i - 1] + 1) {
            return false;
        }
    }
    return g->n > 0 && g->vals[g->n - 1] == last;
}

static void test_empty_and_fill(void)
{
    flash_erase_all();
    got_t g;
    hr_scan_t r = scan(&g);
    CHECK(r.seq == 0 && r.next == 0 && r.count == 0, "blank ring: nothing, write from slot 0");

    writer_t w = {0};
    for (uint32_t v = 1; v <= 10; v++) {
        ring_write(&w, v, SLOT);
    }
    r = scan(&g);
    CHECK(r.seq == 10 && r.next == 10 && r.count == 10 && consecutive_to(&g, 10) && !g.bad_payload,
          "10 records back in order, next slot 10");
}

static void test_wrap(void)
{
    flash_erase_all();
    writer_t w = {0};
    uint32_t total = NSLOTS * 2 + NSLOTS / 2 + 5;  // two and a half times round
    for (uint32_t v = 1; v <= total; v++) {
        ring_write(&w, v, SLOT);
    }
    got_t g;
    hr_scan_t r = scan(&g);
    // The sector being filled was erased at its start: its slots after `next` are blank
    uint32_t blank_in_sector = PER_SEC - w.next % PER_SEC;
    CHECK(r.seq == total && r.next == w.next, "newest #%u found, next %u (writer at %u)", r.seq, r.next, w.next);
    CHECK(r.count == NSLOTS - blank_in_sector && consecutive_to(&g, total) && !g.bad_payload,
          "after the wrap: %u records, oldest first, ending at the newest", r.count);

    // Writing on from the scan result continues the sequence without losing anything
    writer_t w2 = {r.seq, r.next};
    ring_write(&w2, total + 1, SLOT);
    r = scan(&g);
    CHECK(r.seq == total + 1 && consecutive_to(&g, total + 1), "a write after the restart continues the ring");
}

// Power cut in the middle of a slot write: header and part of the payload in flash, the rest 0xFF
static void test_cut_mid_slot(void)
{
    flash_erase_all();
    writer_t w = {0};
    for (uint32_t v = 1; v <= 20; v++) {
        ring_write(&w, v, SLOT);
    }
    ring_write(&w, 21, 24);  // cut after 24 of 64 bytes
    got_t g;
    hr_scan_t r = scan(&g);
    CHECK(r.seq == 20 && consecutive_to(&g, 20), "the cut slot is skipped, #20 is the newest");
    CHECK(r.next == 21, "next write skips the half-written slot 20 (it can't be written without an erase): %u",
          r.next);

    writer_t w2 = {r.seq, r.next};
    ring_write(&w2, 21, SLOT);
    r = scan(&g);
    CHECK(r.seq == 21 && g.n == 21 && g.vals[20] == 21 && !g.bad_payload, "the next record is stored intact");

    // Cut on the last slot of a sector: the skip ends at the next sector start (erased by the write)
    flash_erase_all();
    w = (writer_t){0};
    for (uint32_t v = 1; v <= PER_SEC - 1; v++) {
        ring_write(&w, v, SLOT);
    }
    ring_write(&w, PER_SEC, 10);
    r = scan(&g);
    CHECK(r.seq == PER_SEC - 1 && r.next == PER_SEC, "cut in the sector's last slot: next is the new sector (%u)",
          r.next);
}

// A bit flipped in a stored record (or a slot left inconsistent): only that record is lost
static void test_bad_crc(void)
{
    flash_erase_all();
    writer_t w = {0};
    for (uint32_t v = 1; v <= 50; v++) {
        ring_write(&w, v, SLOT);
    }
    g_flash[30 * SLOT + 20] ^= 0x04;  // record #31
    got_t g;
    hr_scan_t r = scan(&g);
    CHECK(r.seq == 50 && r.count == 49 && !g.bad_payload, "49 of 50 restored, the damaged one skipped");
    CHECK(g.vals[29] == 30 && g.vals[30] == 32 && g.vals[48] == 50, "order kept around the gap");

    // The newest one damaged: the one before it is the newest (used for the PI restore)
    g_flash[49 * SLOT + 9] ^= 0x80;
    r = scan(&g);
    CHECK(r.seq == 49 && g.vals[g.n - 1] == 49, "damaged newest: #49 is used as the newest");
    CHECK(r.next == 50, "and the damaged slot is not written over (%u)", r.next);
}

// Power cut during the erase of the next sector: part of it still holds records from the previous lap
static void test_cut_mid_erase(void)
{
    flash_erase_all();
    writer_t w = {0};
    uint32_t total = NSLOTS + PER_SEC;  // the ring has wrapped; the next write erases sector 1
    for (uint32_t v = 1; v <= total; v++) {
        ring_write(&w, v, SLOT);
    }
    CHECK(w.next == PER_SEC, "writer at the start of sector 1");
    // Half-erased: the first 20 slots are blank, slot 20 is half cleared, the rest are the old records
    memset(g_flash + PER_SEC * SLOT, 0xFF, 20 * SLOT + SLOT / 2);
    got_t g;
    hr_scan_t r = scan(&g);
    CHECK(r.seq == total && r.next == PER_SEC, "newest found; the next write starts the erase again");
    CHECK(consecutive_to(&g, total) && !g.bad_payload, "old records left in the half-erased sector come first, in order"
          " (%d restored)", g.n);
    CHECK(g.n == NSLOTS - 21, "only the erased and the half-erased slots are missing (%d)", g.n);
}

// A ring written by an older firmware with a different payload size: nothing passes the CRC
static void test_foreign_data(void)
{
    for (size_t i = 0; i < sizeof(g_flash); i++) {
        g_flash[i] = (uint8_t)(i * 31 + 7);
    }
    got_t g;
    hr_scan_t r = scan(&g);
    CHECK(r.seq == 0 && r.count == 0 && r.next == 0, "garbage: empty ring, write from slot 0 (sector start, erased)");
}

int main(void)
{
    test_empty_and_fill();
    test_wrap();
    test_cut_mid_slot();
    test_bad_crc();
    test_cut_mid_erase();
    test_foreign_data();
    TEST_DONE();
}
