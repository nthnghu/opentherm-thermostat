#include "sensor_filter.h"
#include <math.h>
#include <string.h>

void sf_init(sensor_filter_t *f, int stale_s)
{
    memset(f, 0, sizeof(*f));
    f->stale_us = (int64_t)stale_s * 1000000;
    f->age_s = -1;
    f->last_t = -1000;
    f->last_h = -1000;
}

bool sf_check(sensor_filter_t *f, bool read_ok, float t, float h, sf_action_t *action, bool *stuck_now)
{
    *action = SF_ACT_NONE;
    *stuck_now = false;
    f->fails_in_row = read_ok ? 0 : f->fails_in_row + 1;
    if (f->fails_in_row == 5) {
        *action = SF_ACT_SOFT_RESET;
    } else if (f->fails_in_row >= 15 && f->fails_in_row % 15 == 0) {
        *action = SF_ACT_BUS_RESET;
    }
    if (!read_ok) {
        return false;
    }
    // A real sensor never repeats the exact same pair of readings for long (its last digits are noise)
    f->same = (t == f->last_t && h == f->last_h) ? f->same + 1 : 0;
    f->last_t = t;
    f->last_h = h;
    if (f->same >= SF_STUCK_N) {
        *stuck_now = f->same == SF_STUCK_N;
        return false;
    }
    return true;
}

bool sf_accept(sensor_filter_t *f, int64_t now_us, bool ok, float t, float h)
{
    bool good = ok && t > SF_T_MIN && t < SF_T_MAX && isfinite(h);  // NaN fails the range test too
    if (good) {
        f->raw = t;
        bool restart = !f->last_ok_us || now_us - f->last_ok_us > f->stale_us;
        if (restart) {
            f->room_c = t;
            f->humidity = h;
        } else {
            f->room_c += SF_SMOOTH_ALPHA * (t - f->room_c);
            f->humidity += SF_SMOOTH_ALPHA * (h - f->humidity);
        }
        // Dead band for the display: a value sitting on a 0.05 border no longer flips back and forth
        if (restart || fabsf(f->room_c - f->room_shown) >= SF_SHOW_BAND_C) {
            f->room_shown = roundf(f->room_c * 10) / 10;
        }
        if (restart || fabsf(f->humidity - f->humidity_shown) >= SF_SHOW_BAND_RH) {
            f->humidity_shown = roundf(f->humidity);
        }
        f->last_ok_us = now_us;
    } else {
        f->errors++;
    }
    f->ok = f->last_ok_us && now_us - f->last_ok_us < f->stale_us;
    f->age_s = f->last_ok_us ? (int)((now_us - f->last_ok_us) / 1000000) : -1;
    return good;
}

void sf_shift(sensor_filter_t *f, float d)
{
    f->room_c += d;
    f->raw += d;
    f->room_shown = roundf((f->room_shown + d) * 10) / 10;
}
