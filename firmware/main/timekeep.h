#pragma once
// The time zones offered on the device (Settings > Clock) and on the web page: a name and its POSIX TZ string.
typedef struct {
    const char *name, *tz;
} timekeep_zone_t;
extern const timekeep_zone_t TIMEKEEP_ZONES[];
extern const int TIMEKEEP_ZONE_COUNT;

// Wall clock: NTP (default once Wi-Fi is up) or set by hand. No battery RTC on this board, so a
// manually set time survives a soft reset but not a power cut.
#include <stdbool.h>
#include <time.h>

typedef enum { TIME_SRC_NONE, TIME_SRC_MANUAL, TIME_SRC_NTP } time_src_t;

void timekeep_init(void);           // applies TZ, starts SNTP if enabled (call after Wi-Fi start)
void timekeep_apply_settings(void); // after the NTP/TZ settings changed
bool timekeep_valid(void);
time_src_t timekeep_source(void);
const char *timekeep_source_name(void);
void timekeep_set_manual(const struct tm *local);
