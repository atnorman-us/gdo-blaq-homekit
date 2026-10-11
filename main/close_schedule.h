// close_schedule.h
//
// Pure (hardware-free) logic for the scheduled close: day math, the
// "should we fire now" decision, and validation/parsing of user input.
// Header-only so tests/test_close_schedule.py can compile it on the host.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "form_parse.h"

#define CLOSE_SCHEDULE_WINDOW_MIN        5
#define CLOSE_SCHEDULE_DEFAULT_MIN       (23 * 60)
#define CLOSE_SCHEDULE_DEFAULT_TZ        "UTC0"
#define CLOSE_SCHEDULE_MIN_VALID_EPOCH   1704067200   // 2024-01-01T00:00:00Z
#define CLOSE_SCHEDULE_TZ_MAX_LEN        63

typedef struct {
    bool     enabled;
    uint16_t minute;      // minutes after local midnight, 0..1439
    char     tz[64];      // POSIX TZ string, NUL-terminated
} gdo_close_schedule_t;

// Days since 1970-01-01 for a civil (local) date. Howard Hinnant's algorithm.
static inline int32_t close_schedule_days_from_civil(int year, int month, int day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153 * (unsigned)(month + (month > 2 ? -3 : 9)) + 2) / 5 + (unsigned)day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int32_t)(era * 146097 + (int)doe - 719468);
}

// The local day on which the window containing `now_min` started. A window
// that began at 23:58 and is still open at 00:01 belongs to the previous day,
// which is what keeps it from firing twice across midnight.
static inline int32_t close_schedule_window_day(uint16_t now_min, int32_t today, uint16_t sched_min)
{
    return now_min >= sched_min ? today : today - 1;
}

// door_open_for_ms is 0 when the door is not open. The door must already have
// been open when the scheduled minute began: the clock has minute granularity,
// so real elapsed time since the window start is < (since + 1) * 60 s, and a
// door open at least that long was provably open before the window. This can
// miss a door opened less than 60 s before the scheduled minute (acceptable),
// but never fires for a door opened after the scheduled time.
static inline bool close_schedule_should_fire(bool clock_synced, int64_t door_open_for_ms,
                                              uint16_t now_min, int32_t today,
                                              uint16_t sched_min, int32_t last_fired_day)
{
    if (!clock_synced || door_open_for_ms <= 0) return false;
    if (now_min >= 1440 || sched_min >= 1440) return false;
    unsigned since = ((unsigned)now_min + 1440u - (unsigned)sched_min) % 1440u;
    if (since >= CLOSE_SCHEDULE_WINDOW_MIN) return false;
    if (door_open_for_ms < (int64_t)(since + 1) * 60000) return false;
    return last_fired_day != close_schedule_window_day(now_min, today, sched_min);
}

// "HH:MM" (24h, zero padded) -> minutes after midnight.
static inline bool close_schedule_parse_hhmm(const char *s, uint16_t *minute)
{
    if (!s || !minute || strlen(s) != 5 || s[2] != ':') return false;
    const int idx[4] = {0, 1, 3, 4};
    for (int i = 0; i < 4; i++) {
        if (s[idx[i]] < '0' || s[idx[i]] > '9') return false;
    }
    int h = (s[0] - '0') * 10 + (s[1] - '0');
    int m = (s[3] - '0') * 10 + (s[4] - '0');
    if (h > 23 || m > 59) return false;
    *minute = (uint16_t)(h * 60 + m);
    return true;
}

static inline bool close_schedule_tz_valid(const char *tz)
{
    if (!tz) return false;
    size_t n = strlen(tz);
    if (n == 0 || n > CLOSE_SCHEDULE_TZ_MAX_LEN) return false;
    char first = tz[0];
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '<')) return false;
    for (size_t i = 0; i < n; i++) {
        char c = tz[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == ',' || c == '.' || c == ':' || c == '/' || c == '+' || c == '-' ||
                  c == '<' || c == '>' || c == '_';
        if (!ok) return false;
    }
    return true;
}

typedef enum {
    CLOSE_SCHEDULE_FORM_OK = 0,
    CLOSE_SCHEDULE_FORM_BAD_TIME,
    CLOSE_SCHEDULE_FORM_BAD_TZ,
} close_schedule_form_result_t;

// Body: "enabled=1&time=23%3A00&tz=PST8PDT%2CM3.2.0%2CM11.1.0".
// A missing "enabled" means disabled.
static inline close_schedule_form_result_t close_schedule_parse_form(const char *body,
                                                                     gdo_close_schedule_t *out)
{
    char flag[4] = {0};
    char hhmm[8] = {0};
    memset(out, 0, sizeof(*out));
    out->enabled = form_get_param(body, "enabled", flag, sizeof(flag)) &&
                   flag[0] == '1' && flag[1] == '\0';
    if (!form_get_param(body, "time", hhmm, sizeof(hhmm)) ||
        !close_schedule_parse_hhmm(hhmm, &out->minute)) {
        return CLOSE_SCHEDULE_FORM_BAD_TIME;
    }
    if (!form_get_param(body, "tz", out->tz, sizeof(out->tz)) ||
        !close_schedule_tz_valid(out->tz)) {
        return CLOSE_SCHEDULE_FORM_BAD_TZ;
    }
    return CLOSE_SCHEDULE_FORM_OK;
}
