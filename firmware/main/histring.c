#include "histring.h"
#include <string.h>

uint16_t hr_crc16(const uint8_t *p, size_t n, uint16_t c)
{
    while (n--) {
        c ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) {
            c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
        }
    }
    return c;
}

static uint16_t slot_crc(uint32_t seq, const void *payload, size_t n)
{
    return hr_crc16((const uint8_t *)payload, n, hr_crc16((const uint8_t *)&seq, sizeof(seq), 0xFFFF));
}

void hr_make_slot(uint8_t *slot, size_t slot_size, uint32_t seq, const void *payload, size_t payload_size)
{
    memset(slot, 0, slot_size);
    hr_hdr_t h = {.magic = HR_MAGIC, .seq = seq, .crc = slot_crc(seq, payload, payload_size)};
    memcpy(slot, &h, sizeof(h));
    memcpy(slot + sizeof(h), payload, payload_size);
}

bool hr_slot_valid(const uint8_t *slot, size_t payload_size)
{
    hr_hdr_t h;
    memcpy(&h, slot, sizeof(h));
    return h.magic == HR_MAGIC && h.seq != 0 && h.seq != 0xFFFFFFFF &&
           h.crc == slot_crc(h.seq, slot + sizeof(h), payload_size);
}

static uint32_t slot_seq(const uint8_t *slot)
{
    hr_hdr_t h;
    memcpy(&h, slot, sizeof(h));
    return h.seq;
}

static bool blank(const uint8_t *slot, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (slot[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

hr_scan_t hr_scan(const uint8_t *buf, uint32_t nslots, uint32_t slot_size, uint32_t payload_size,
                  void (*put)(void *ctx, const void *payload), void *ctx)
{
    hr_scan_t r = {0};
    uint32_t max_i = 0;
    for (uint32_t i = 0; i < nslots; i++) {
        const uint8_t *s = buf + (size_t)i * slot_size;
        if (hr_slot_valid(s, payload_size) && slot_seq(s) > r.seq) {
            r.seq = slot_seq(s);
            max_i = i;
        }
    }
    uint32_t per_sector = HR_SECTOR / slot_size;
    r.next = r.seq ? (max_i + 1) % nslots : 0;
    // A half-written slot can't be written again without an erase: skip to a blank slot or the next sector start
    while (r.next % per_sector != 0 && !blank(buf + (size_t)r.next * slot_size, slot_size)) {
        r.next = (r.next + 1) % nslots;
    }
    // Oldest first: start after the newest and go round, keeping only slots older than the newest, in order
    uint32_t prev = 0;
    for (uint32_t k = 1; r.seq && k <= nslots; k++) {
        const uint8_t *s = buf + (size_t)((max_i + k) % nslots) * slot_size;
        if (!hr_slot_valid(s, payload_size) || slot_seq(s) <= prev || slot_seq(s) > r.seq) {
            continue;
        }
        prev = slot_seq(s);
        if (put) {
            put(ctx, s + sizeof(hr_hdr_t));
        }
        r.count++;
    }
    return r;
}
