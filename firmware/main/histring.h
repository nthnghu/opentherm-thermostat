#pragma once
// Slot format and restore scan of the flash history rings (pure C, host-tested in tests/host/test_histring.c).
// histstore.c does the flash reads, erases and writes.
//
// A ring is a run of 4 KB sectors cut into equal slots. Each slot: magic, CRC-16 over seq + payload, an increasing
// sequence number, the payload. Writing into the first slot of a sector erases that sector first, so the oldest
// sector is dropped as a whole.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HR_SECTOR 4096
#define HR_MAGIC  0x4853  // "HS"

typedef struct {
    uint16_t magic;
    uint16_t crc;    // CRC-16/CCITT over seq and the payload
    uint32_t seq;    // 1, 2, 3 ... per ring (0xFFFFFFFF = erased)
} hr_hdr_t;

uint16_t hr_crc16(const uint8_t *p, size_t n, uint16_t c);

// Fills one slot (slot_size bytes, the rest after the payload is 0).
void hr_make_slot(uint8_t *slot, size_t slot_size, uint32_t seq, const void *payload, size_t payload_size);

// A complete, intact slot
bool hr_slot_valid(const uint8_t *slot, size_t payload_size);

typedef struct {
    uint32_t seq;    // newest sequence number found (0 = ring empty)
    uint32_t next;   // slot to write next
    uint32_t count;  // valid slots handed to put
} hr_scan_t;

// Scans a whole ring read into buf (nslots x slot_size bytes): calls put(ctx, payload) for every valid slot, oldest
// first, so the last call is the newest. Slots with a bad CRC (power cut mid-write, half-erased sector) are skipped.
// next is the slot after the newest, moved past any slot that is not blank (a half-written one, which can't be
// written again without an erase) up to the next sector start, where the write erases first.
hr_scan_t hr_scan(const uint8_t *buf, uint32_t nslots, uint32_t slot_size, uint32_t payload_size,
                  void (*put)(void *ctx, const void *payload), void *ctx);
