#include "ot_frame.h"

uint32_t ot_frame(uint8_t type, uint8_t id, uint16_t data)
{
    uint32_t f = ((uint32_t)(type & 7) << 28) | ((uint32_t)id << 16) | data;
    if (__builtin_popcount(f) & 1) {
        f |= 1u << 31;  // even parity over all 32 bits
    }
    return f;
}

bool ot_parity_ok(uint32_t f)
{
    return (__builtin_popcount(f) & 1) == 0;
}

const char *ot_type_name(uint8_t type)
{
    static const char *const names[8] = {"READ-DATA", "WRITE-DATA", "INVALID-DATA", "-reserved-",
                                         "READ-ACK",  "WRITE-ACK",  "DATA-INVALID", "UNKNOWN-DATAID"};
    return names[type & 7];
}
