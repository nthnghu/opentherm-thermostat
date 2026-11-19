#pragma once
// OpenTherm Manchester coding (pure C, host-tested in tests/host/test_manchester.c).
// Header-only on purpose: the receive step runs inside the GPIO edge ISR, and a static inline function is
// compiled into that IRAM handler (no call into flash code while the cache may be off).
#include <stdbool.h>
#include <stdint.h>

// always_inline: a plain "static inline" may still be emitted as a separate function in flash
#define OTM_INLINE static inline __attribute__((always_inline))

#define OTM_HALF_BITS 68     // 34 bits (start, 32 data, stop) x 2 half-bits
#define OTM_MID_GAP_US 750   // edges closer than this to the last mid-bit edge are bit-border edges (or noise)

// Line levels for a frame, one per 500 us half-bit. '1' = active then idle, '0' = idle then active.
// Start and stop bits are '1'.
OTM_INLINE void otm_encode(uint32_t frame, uint8_t levels[OTM_HALF_BITS], uint8_t active, uint8_t idle)
{
    int p = 0;
    for (int i = -1; i <= 32; i++) {
        bool bit = (i < 0 || i == 32) ? true : (frame >> (31 - i)) & 1;
        levels[p++] = bit ? active : idle;
        levels[p++] = bit ? idle : active;
    }
}

typedef enum { OTM_WAITING, OTM_START_BIT, OTM_RECEIVING, OTM_READY, OTM_INVALID } otm_state_t;

typedef struct {
    volatile otm_state_t st;
    int64_t ts;              // time of the last mid-bit edge (us)
    int64_t start;           // time of the start bit's first edge
    int bits;
    uint32_t frame;
} otm_rx_t;

OTM_INLINE void otm_rx_begin(otm_rx_t *r)
{
    r->st = OTM_WAITING;
    r->bits = 0;
    r->frame = 0;
}

// One edge on the input (level after the edge, 1 = line active). After the reference Arduino OpenTherm library
// (ihormelnyk): the bit value is the inverted level at the first edge more than 750 us after the previous mid-bit
// edge. Returns true when the frame is finished: st is then OTM_READY (frame holds the 32 bits, parity not yet
// checked) or OTM_INVALID.
OTM_INLINE bool otm_rx_edge(otm_rx_t *r, int64_t now, int level)
{
    switch (r->st) {
    case OTM_WAITING:
        if (level == 1) {
            r->st = OTM_START_BIT;
            r->ts = now;
            r->start = now;
            return false;
        }
        r->st = OTM_INVALID;
        return true;
    case OTM_START_BIT:
        if (now - r->ts < OTM_MID_GAP_US && level == 0) {
            r->st = OTM_RECEIVING;
            r->ts = now;
            r->bits = 0;
            r->frame = 0;
            return false;
        }
        r->st = OTM_INVALID;
        return true;
    case OTM_RECEIVING:
        if (now - r->ts > OTM_MID_GAP_US) {
            if (r->bits < 32) {
                r->frame = (r->frame << 1) | (level ? 0 : 1);
                r->ts = now;
                r->bits++;
            } else {
                r->st = OTM_READY;  // stop bit
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}
