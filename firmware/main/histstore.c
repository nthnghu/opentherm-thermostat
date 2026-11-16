#include "histstore.h"
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "ot_bus.h"

static const char *TAG = "hist";

#define SECTOR 4096
#define MAGIC 0x4853  // "HS"

typedef struct {
    uint16_t magic;
    uint16_t crc;    // CRC-16/CCITT over seq and the payload
    uint32_t seq;    // 1, 2, 3 ... per ring
} slot_hdr_t;

typedef struct {
    const char *name;
    uint32_t first_sector, sectors;
    uint32_t slot;          // bytes per slot (header + payload, power of two)
    uint32_t payload;       // bytes of payload
    // state
    uint32_t next;          // slot index to write next
    uint32_t seq;           // last sequence number written
    uint32_t synced;        // datalog *_seq already in flash
    uint32_t restored;
} ring_t;

_Static_assert(sizeof(slot_hdr_t) + sizeof(dl_rec_t) <= 64, "minute record must fit a 64-byte slot");
_Static_assert(sizeof(slot_hdr_t) + sizeof(dl_burn_t) <= 32, "burn must fit a 32-byte slot");
_Static_assert(sizeof(slot_hdr_t) + sizeof(dl_hour_t) <= 32, "hour must fit a 32-byte slot");

// 26 sectors x 64 = 1664 minutes (24 h + spare), 5 x 128 = 640 burns, 8 x 128 = 1024 hours (35 days + spare)
static ring_t s_rings[3] = {
    {"minutes", 0, 26, 64, sizeof(dl_rec_t)},
    {"burns", 26, 5, 32, sizeof(dl_burn_t)},
    {"hours", 31, 8, 32, sizeof(dl_hour_t)},
};
enum { R_MIN = 0, R_BURN, R_HOUR };

static const esp_partition_t *s_part;
static bool s_have_last;
static dl_rec_t s_last;

static uint16_t crc16(const uint8_t *p, size_t n, uint16_t c)
{
    while (n--) {
        c ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) {
            c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
        }
    }
    return c;
}

static uint32_t nslots(const ring_t *r)
{
    return r->sectors * SECTOR / r->slot;
}

static uint32_t addr(const ring_t *r, uint32_t i)
{
    return r->first_sector * SECTOR + i * r->slot;
}

static uint16_t slot_crc(uint32_t seq, const void *payload, size_t n)
{
    return crc16((const uint8_t *)payload, n, crc16((const uint8_t *)&seq, sizeof(seq), 0xFFFF));
}

// Reads a whole ring; calls put(payload) for every valid slot, oldest first. Sets next and seq.
static void scan(ring_t *r, void (*put)(dl_t *, const void *), dl_t *d)
{
    uint32_t n = nslots(r), max_seq = 0, max_i = 0;
    size_t bytes = r->sectors * SECTOR;
    uint8_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!buf || esp_partition_read(s_part, r->first_sector * SECTOR, buf, bytes) != ESP_OK) {
        free(buf);
        ESP_LOGE(TAG, "%s: read failed", r->name);
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        const slot_hdr_t *h = (const slot_hdr_t *)(buf + i * r->slot);
        if (h->magic == MAGIC && h->seq != 0xFFFFFFFF &&
            h->crc == slot_crc(h->seq, h + 1, r->payload) && h->seq > max_seq) {
            max_seq = h->seq;
            max_i = i;
        }
    }
    r->seq = max_seq;
    r->next = max_seq ? (max_i + 1) % n : 0;
    // Oldest first: start after the newest and go round; keep only slots older than the newest, in order.
    uint32_t prev = 0, count = 0;
    for (uint32_t k = 1; max_seq && k <= n; k++) {
        uint32_t i = (max_i + k) % n;
        const slot_hdr_t *h = (const slot_hdr_t *)(buf + i * r->slot);
        if (h->magic != MAGIC || h->seq == 0xFFFFFFFF || h->seq <= prev || h->seq > max_seq ||
            h->crc != slot_crc(h->seq, h + 1, r->payload)) {
            continue;
        }
        prev = h->seq;
        if (put) {
            put(d, h + 1);
        }
        if (r == &s_rings[R_MIN]) {
            memcpy(&s_last, h + 1, sizeof(s_last));
            s_have_last = true;
        }
        count++;
    }
    r->restored = count;
    free(buf);
}

static bool write_slot(ring_t *r, const void *payload)
{
    uint8_t slot[64] = {0};
    slot_hdr_t *h = (slot_hdr_t *)slot;
    h->magic = MAGIC;
    h->seq = r->seq + 1;
    memcpy(h + 1, payload, r->payload);
    h->crc = slot_crc(h->seq, h + 1, r->payload);
    uint32_t a = addr(r, r->next);
    bool quiet = otbus_quiet_begin(2000);  // between OpenTherm frames
    esp_err_t err = ESP_OK;
    if (a % SECTOR == 0) {
        err = esp_partition_erase_range(s_part, a, SECTOR);
    }
    if (err == ESP_OK) {
        err = esp_partition_write(s_part, a, slot, r->slot);
    }
    if (quiet) {
        otbus_quiet_end();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: write at 0x%lx failed: %s", r->name, (unsigned long)a, esp_err_to_name(err));
        return false;
    }
    r->seq++;
    r->next = (r->next + 1) % nslots(r);
    return true;
}

bool histstore_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "hist");
    if (!s_part) {
        ESP_LOGW(TAG, "no \"hist\" partition: history is kept in RAM only (flash the partition table over USB)");
        return false;
    }
    uint32_t need = (s_rings[R_HOUR].first_sector + s_rings[R_HOUR].sectors) * SECTOR;
    if (s_part->size < need) {
        ESP_LOGE(TAG, "\"hist\" partition too small (%lu < %lu)", (unsigned long)s_part->size, (unsigned long)need);
        s_part = NULL;
        return false;
    }
    return true;
}

bool histstore_ok(void)
{
    return s_part != NULL;
}

static void put_rec(dl_t *d, const void *p)
{
    datalog_restore_rec(d, (const dl_rec_t *)p);
}

static void put_burn(dl_t *d, const void *p)
{
    datalog_restore_burn(d, (const dl_burn_t *)p);
}

static void put_hour(dl_t *d, const void *p)
{
    datalog_restore_hour(d, (const dl_hour_t *)p);
}

void histstore_restore(dl_t *d)
{
    if (!s_part) {
        return;
    }
    scan(&s_rings[R_MIN], put_rec, d);
    scan(&s_rings[R_BURN], put_burn, d);
    scan(&s_rings[R_HOUR], put_hour, d);
    // What is in RAM now is in flash already
    s_rings[R_MIN].synced = d->rec_seq;
    s_rings[R_BURN].synced = d->burn_seq;
    s_rings[R_HOUR].synced = d->hour_seq;
    ESP_LOGI(TAG, "restored %lu minutes, %lu burns, %lu hours", (unsigned long)s_rings[R_MIN].restored,
             (unsigned long)s_rings[R_BURN].restored, (unsigned long)s_rings[R_HOUR].restored);
}

void histstore_sync(const dl_t *d)
{
    if (!s_part) {
        return;
    }
    // Normally one new entry per call; after a long stall write only what RAM still holds.
    ring_t *r = &s_rings[R_MIN];
    for (uint32_t k = d->rec_seq - r->synced; k > 0; k--) {
        dl_rec_t rec;
        if (k <= (uint32_t)datalog_count(d) && datalog_get(d, datalog_count(d) - (int)k, &rec)) {
            write_slot(r, &rec);
        }
    }
    r->synced = d->rec_seq;
    r = &s_rings[R_BURN];
    for (uint32_t k = d->burn_seq - r->synced; k > 0; k--) {
        dl_burn_t b;
        if (k <= (uint32_t)datalog_burn_count(d) && datalog_get_burn(d, datalog_burn_count(d) - (int)k, &b)) {
            write_slot(r, &b);
        }
    }
    r->synced = d->burn_seq;
    r = &s_rings[R_HOUR];
    for (uint32_t k = d->hour_seq - r->synced; k > 0; k--) {
        dl_hour_t h;
        if (k <= (uint32_t)datalog_hour_count(d) && datalog_get_hour(d, datalog_hour_count(d) - (int)k, &h)) {
            write_slot(r, &h);
        }
    }
    r->synced = d->hour_seq;
}

bool histstore_last_rec(dl_rec_t *out)
{
    if (s_have_last) {
        *out = s_last;
    }
    return s_have_last;
}

void histstore_status(char *out, size_t size)
{
    if (!s_part) {
        snprintf(out, size, "RAM only (no flash partition)");
        return;
    }
    snprintf(out, size, "flash: restored %lu minutes, %lu burns, %lu hours; written up to #%lu/#%lu/#%lu",
             (unsigned long)s_rings[R_MIN].restored, (unsigned long)s_rings[R_BURN].restored,
             (unsigned long)s_rings[R_HOUR].restored, (unsigned long)s_rings[R_MIN].seq,
             (unsigned long)s_rings[R_BURN].seq, (unsigned long)s_rings[R_HOUR].seq);
}
