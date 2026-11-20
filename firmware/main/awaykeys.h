#pragma once
// Away mode is stored in two NVS keys, "away" (flag) and "awayuntil" (end time, 0 = open-ended). Each key is
// written to flash on its own, so a power cut can land between them (REVIEW.md T17). The order below makes the
// state loaded after such a cut either the old one or the new one, never a mix:
//   switching on (or changing the end): the end time first, then the flag
//   switching off: the flag first, then the end time
// The old fixed order (end time, then flag) turned "away until Friday -> off" into "away for good" when cut.
#include <stdbool.h>
#include <stdint.h>

typedef enum { AWAY_KEY_FLAG, AWAY_KEY_UNTIL } away_key_t;

static inline void away_save_order(bool on, away_key_t order[2])
{
    order[0] = on ? AWAY_KEY_UNTIL : AWAY_KEY_FLAG;
    order[1] = on ? AWAY_KEY_FLAG : AWAY_KEY_UNTIL;
}

// Which state the stored keys mean. An end time must be 0 (open-ended) or a plausible date (2025..2100), not a
// damaged value; a flag without a readable end time is not away.
static inline bool away_load(bool have_flag, uint8_t flag, bool have_until, int64_t until, int64_t *out_until)
{
    if (!have_flag || !flag || !have_until || !(until == 0 || (until > 1735689600LL && until < 4102444800LL))) {
        *out_until = 0;
        return false;
    }
    *out_until = until;
    return true;
}
