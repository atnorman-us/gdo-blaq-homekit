# Scheduled Close Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** If the garage door is open at a user-set time of day (e.g. 23:00), the device closes it automatically, using the same warning-then-close sequence as the existing auto-close.

**Architecture:** Pure, host-testable logic lives in two header-only files (`close_schedule.h`, `form_parse.h`). A small `clock_sync` module wraps SNTP and the POSIX timezone. Settings persist in NVS via accessors in `gdo-blaq-homekit.cpp` (same pattern as auto-close). The existing 5-second watchdog tick calls a new `sched_close_check()`, which reuses `pre_close_warning_run_async()` and `gdo_door_close()`. The diagnostics page gets a "Scheduled Close" card and a new `POST /settings/close-schedule`.

**Tech Stack:** ESP-IDF v5.4.1 (C/C++), `esp_netif_sntp`, NVS, `esp_http_server`; host tests are Python `unittest` that compile extracted C with `cc -fsanitize=address,undefined` (see `tests/test_regressions.py`: `function()`, `run_c()`).

**Spec:** `docs/superpowers/specs/2026-10-11-scheduled-close-design.md`

## Global Constraints

- SNTP server `pool.ntp.org`; the clock counts as synced only if `time(NULL) >= 1704067200` (2024-01-01 UTC).
- Schedule is off by default; default time 23:00 (1380); default timezone `UTC0`.
- One daily time, one enable switch. No per-day or multiple times, no HomeKit exposure, no notifications.
- Fire window is 5 minutes: `[sched_minute, sched_minute + 5)`, wrapping at midnight. At most one fire per window.
- NVS namespace `gdo_settings`; keys `sched_en` (u8), `sched_min` (u16), `tz` (string), `sched_last_day` (i32). NVS key names are max 15 chars.
- Timezone string: 1..63 chars, first char a letter or `<`, remaining chars from `A-Za-z0-9 , . : / + - < > _` (no spaces, quotes, or backslashes, which also keeps it safe to embed in `/status` JSON).
- `POST /settings/close-schedule` must call `authorize_mutation(req)` first, like every other settings POST.
- Closing must go through `pre_close_warning_run_async(PRE_CLOSE_WARNING_DURATION_MS, cb)`, then `gdo_door_close()` followed by `notify_homekit_target_door_state_change(TGT_CLOSED)`, and must be skipped unless the door is OPEN, synced, and obstruction is Clear. Revalidate in the callback.
- No attribution trailers in commit messages (user rule).
- Run the full suite before each commit: `python3 -m unittest discover -s tests -v` (baseline: 21 tests pass).

## Review Focus

1. Schedule window straddling midnight (e.g. 23:58): must fire once, not twice (before and after 00:00).
2. Reboot or Wi-Fi drop inside the window after it already fired: must not fire again (`sched_last_day`).
3. Clock not yet synced after boot (SNTP slow or offline): must never fire, never fire off a 1970 clock.
4. Obstruction not Clear at the scheduled time: must hold off and retry on later ticks inside the window, without burning the day.
5. Timezone or time input from the browser with special characters (`+`, `,`, `/`, `<`, `>`, `%2B` encodings, `%00`): must decode correctly or be rejected, never truncate or overflow.

(Each line is pinned by a test in the task named in brackets: 1,2 [Task 1 and 4], 3 [Task 1 and 4], 4 [Task 4], 5 [Task 1 and 5].)

---

## File Structure

- Create `main/form_parse.h`: header-only `form_get_param()` (urlencoded body lookup with percent-decoding).
- Create `main/close_schedule.h`: header-only types and pure logic (`gdo_close_schedule_t`, day math, window logic, time and timezone validation, form parsing).
- Create `main/clock_sync.h`, `main/clock_sync.cpp`: SNTP start, apply timezone, synced check, local time helpers.
- Modify `main/gdo_settings.h`: include `close_schedule.h`, declare get/set accessors.
- Modify `main/gdo-blaq-homekit.cpp`: NVS load/set, `sched_close_check()`, completion callback, watchdog hook, `app_main` load call.
- Modify `main/homekit.cpp`: call `clock_sync_start()` after `app_wifi_init()`.
- Modify `main/diag_webserver.cpp`: POST handler and registration, `/status` fields, UI card and JS.
- Modify `main/CMakeLists.txt`: add `clock_sync.cpp`.
- Modify `README.md`: short user-facing section.
- Create `tests/test_close_schedule.py`, `tests/test_sched_close.py`.

---

### Task 1: Pure logic headers (`form_parse.h`, `close_schedule.h`)

**Files:**
- Create: `main/form_parse.h`
- Create: `main/close_schedule.h`
- Test: `tests/test_close_schedule.py`

**Interfaces:**
- Consumes: nothing.
- Produces (all `static inline`, C-compatible):
  - `bool form_get_param(const char *body, const char *key, char *out, size_t out_len)`
  - `typedef struct { bool enabled; uint16_t minute; char tz[64]; } gdo_close_schedule_t;`
  - `int32_t close_schedule_days_from_civil(int year, int month, int day)` (days since 1970-01-01)
  - `int32_t close_schedule_window_day(uint16_t now_min, int32_t today, uint16_t sched_min)`
  - `bool close_schedule_should_fire(bool clock_synced, bool door_open, uint16_t now_min, int32_t today, uint16_t sched_min, int32_t last_fired_day)`
  - `bool close_schedule_parse_hhmm(const char *s, uint16_t *minute)`
  - `bool close_schedule_tz_valid(const char *tz)`
  - `close_schedule_form_result_t close_schedule_parse_form(const char *body, gdo_close_schedule_t *out)` returning `CLOSE_SCHEDULE_FORM_OK`, `CLOSE_SCHEDULE_FORM_BAD_TIME`, or `CLOSE_SCHEDULE_FORM_BAD_TZ`
  - Macros: `CLOSE_SCHEDULE_WINDOW_MIN` (5), `CLOSE_SCHEDULE_DEFAULT_MIN` (1380), `CLOSE_SCHEDULE_DEFAULT_TZ` ("UTC0"), `CLOSE_SCHEDULE_MIN_VALID_EPOCH` (1704067200), `CLOSE_SCHEDULE_TZ_MAX_LEN` (63)

- [ ] **Step 1: Write the failing tests**

Create `tests/test_close_schedule.py`:

```python
"""Host tests for the pure scheduled-close logic and form parsing."""
import unittest
from test_regressions import ROOT, run_c

INC = ('#include <assert.h>\n#include <string.h>\n'
       '#include "' + str(ROOT / 'main/close_schedule.h') + '"\n')


class CloseScheduleTests(unittest.TestCase):
    def test_days_from_civil(self):
        run_c(INC + r'''
int main(void) {
 assert(close_schedule_days_from_civil(1970,1,1)==0);
 assert(close_schedule_days_from_civil(2000,1,1)==10957);
 assert(close_schedule_days_from_civil(2024,1,1)==19723);
 assert(close_schedule_days_from_civil(2024,12,31)==20088);
 assert(close_schedule_days_from_civil(2025,1,1)==20089);
 assert(close_schedule_days_from_civil(2025,1,1)-close_schedule_days_from_civil(2024,12,31)==1);
 return 0;
}''')

    def test_should_fire_window_edges(self):
        run_c(INC + r'''
int main(void) {
 const uint16_t S = 23*60;      /* 23:00 */
 const int32_t D = 20000, NONE = 0;
 /* bool synced, door_open, now_min, today, sched_min, last_fired */
 assert( close_schedule_should_fire(true,true,S,   D,S,NONE));
 assert( close_schedule_should_fire(true,true,S+4, D,S,NONE));
 assert(!close_schedule_should_fire(true,true,S+5, D,S,NONE));
 assert(!close_schedule_should_fire(true,true,S-1, D,S,NONE));
 return 0;
}''')

    def test_should_fire_requires_open_synced_and_not_already_fired(self):
        run_c(INC + r'''
int main(void) {
 const uint16_t S = 23*60;
 const int32_t D = 20000;
 assert(!close_schedule_should_fire(true,false,S+1,D,S,0));   /* door not open */
 assert(!close_schedule_should_fire(false,true,S+1,D,S,0));   /* clock unsynced */
 assert(!close_schedule_should_fire(true,true,S+2,D,S,D));    /* already fired today */
 assert( close_schedule_should_fire(true,true,S+2,D,S,D-1));  /* fired yesterday only */
 assert(!close_schedule_should_fire(true,true,1440,D,S,0));   /* invalid now */
 assert(!close_schedule_should_fire(true,true,S,D,1440,0));   /* invalid schedule */
 return 0;
}''')

    def test_window_across_midnight_fires_once(self):
        run_c(INC + r'''
int main(void) {
 const uint16_t S = 23*60 + 58;   /* 23:58, window ends 00:03 */
 const int32_t D = 20000;
 /* Fires at 23:59 on day D, records window day D. */
 assert(close_schedule_should_fire(true,true,23*60+59,D,S,0));
 assert(close_schedule_window_day(23*60+59,D,S)==D);
 /* At 00:01 on day D+1 the same window is still open: must NOT fire again. */
 assert(close_schedule_window_day(1,D+1,S)==D);
 assert(!close_schedule_should_fire(true,true,1,D+1,S,D));
 /* A fresh window the next night fires. */
 assert(close_schedule_should_fire(true,true,23*60+58,D+1,S,D));
 /* 00:03 is out of the window. */
 assert(!close_schedule_should_fire(true,true,3,D+1,S,0));
 return 0;
}''')

    def test_parse_hhmm(self):
        run_c(INC + r'''
int main(void) {
 uint16_t m = 9999;
 assert(close_schedule_parse_hhmm("23:00",&m) && m==1380);
 assert(close_schedule_parse_hhmm("00:00",&m) && m==0);
 assert(close_schedule_parse_hhmm("23:59",&m) && m==1439);
 assert(!close_schedule_parse_hhmm("24:00",&m));
 assert(!close_schedule_parse_hhmm("12:60",&m));
 assert(!close_schedule_parse_hhmm("1:00",&m));
 assert(!close_schedule_parse_hhmm("12-00",&m));
 assert(!close_schedule_parse_hhmm("ab:cd",&m));
 assert(!close_schedule_parse_hhmm("",&m));
 assert(!close_schedule_parse_hhmm(NULL,&m));
 return 0;
}''')

    def test_tz_valid(self):
        run_c(INC + r'''
int main(void) {
 assert(close_schedule_tz_valid("UTC0"));
 assert(close_schedule_tz_valid("PST8PDT,M3.2.0,M11.1.0"));
 assert(close_schedule_tz_valid("CET-1CEST,M3.5.0,M10.5.0/3"));
 assert(close_schedule_tz_valid("<+0530>-5:30"));
 assert(!close_schedule_tz_valid(""));
 assert(!close_schedule_tz_valid(NULL));
 assert(!close_schedule_tz_valid("1UTC"));          /* must start with letter or < */
 assert(!close_schedule_tz_valid("UTC 0"));         /* no spaces */
 assert(!close_schedule_tz_valid("UTC\"0"));        /* no quotes */
 assert(!close_schedule_tz_valid("UTC\\0"));        /* no backslash */
 char longtz[80]; memset(longtz,'A',79); longtz[79]=0;
 assert(!close_schedule_tz_valid(longtz));
 char ok63[64]; memset(ok63,'A',63); ok63[63]=0;
 assert(close_schedule_tz_valid(ok63));
 return 0;
}''')

    def test_form_get_param_decodes_and_rejects(self):
        run_c(INC + r'''
int main(void) {
 char out[16];
 assert(form_get_param("a=1&tz=PST8PDT%2CM3.2.0&b=2","tz",out,sizeof out) && !strcmp(out,"PST8PDT,M3.2.0"));
 assert(form_get_param("time=23%3A00","time",out,sizeof out) && !strcmp(out,"23:00"));
 assert(form_get_param("x=%3C%2B0530%3E-5%3A30","x",out,sizeof out) && !strcmp(out,"<+0530>-5:30"));
 assert(form_get_param("x=a+b","x",out,sizeof out) && !strcmp(out,"a b"));
 assert(!form_get_param("xtz=1","tz",out,sizeof out));      /* key must match whole name */
 assert(!form_get_param("tz=%00abc","tz",out,sizeof out));  /* encoded NUL rejected */
 assert(!form_get_param("tz=0123456789abcdefXYZ","tz",out,sizeof out)); /* too long for out */
 assert(form_get_param("tz=%zz","tz",out,sizeof out) && !strcmp(out,"%zz")); /* bad escape kept literal */
 assert(!form_get_param("a=1","tz",out,sizeof out));
 return 0;
}''')

    def test_parse_form(self):
        run_c(INC + r'''
int main(void) {
 gdo_close_schedule_t s;
 assert(close_schedule_parse_form("enabled=1&time=23%3A00&tz=PST8PDT%2CM3.2.0%2CM11.1.0",&s)==CLOSE_SCHEDULE_FORM_OK);
 assert(s.enabled && s.minute==1380 && !strcmp(s.tz,"PST8PDT,M3.2.0,M11.1.0"));
 assert(close_schedule_parse_form("time=05%3A30&tz=UTC0",&s)==CLOSE_SCHEDULE_FORM_OK);
 assert(!s.enabled && s.minute==330);
 assert(close_schedule_parse_form("enabled=1&time=25%3A00&tz=UTC0",&s)==CLOSE_SCHEDULE_FORM_BAD_TIME);
 assert(close_schedule_parse_form("enabled=1&tz=UTC0",&s)==CLOSE_SCHEDULE_FORM_BAD_TIME);
 assert(close_schedule_parse_form("enabled=1&time=23%3A00&tz=bad%20tz",&s)==CLOSE_SCHEDULE_FORM_BAD_TZ);
 assert(close_schedule_parse_form("enabled=1&time=23%3A00",&s)==CLOSE_SCHEDULE_FORM_BAD_TZ);
 return 0;
}''')


if __name__ == '__main__':
    unittest.main()
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python3 -m unittest tests.test_close_schedule -v` from the repo root (or `cd tests && python3 -m unittest test_close_schedule -v`)
Expected: FAIL (compile error: `main/close_schedule.h` not found).

- [ ] **Step 3: Write `main/form_parse.h`**

```c
// form_parse.h
//
// Minimal application/x-www-form-urlencoded lookup, header-only so it can be
// compiled by the host tests without any ESP-IDF headers.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline int form_parse_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Finds `key` (whole-name match) in `body` and copies its percent-decoded
// value into `out`. Returns false if the key is missing, the value does not
// fit in out_len (including the terminator), or it decodes to a NUL byte.
// '+' decodes to ' '. An invalid %-escape is kept literally.
static inline bool form_get_param(const char *body, const char *key, char *out, size_t out_len)
{
    if (!body || !key || !out || out_len == 0) return false;
    size_t klen = strlen(key);
    const char *p = body;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t o = 0;
            while (*p && *p != '&') {
                char c = *p;
                int hi, lo;
                if (c == '%' && (hi = form_parse_hexval(p[1])) >= 0 &&
                    (lo = form_parse_hexval(p[2])) >= 0) {
                    c = (char)(hi * 16 + lo);
                    p += 2;
                } else if (c == '+') {
                    c = ' ';
                }
                if (c == '\0') return false;
                if (o + 1 >= out_len) return false;
                out[o++] = c;
                p++;
            }
            out[o] = '\0';
            return true;
        }
        const char *amp = strchr(p, '&');
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}
```

- [ ] **Step 4: Write `main/close_schedule.h`**

```c
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

static inline bool close_schedule_should_fire(bool clock_synced, bool door_open,
                                              uint16_t now_min, int32_t today,
                                              uint16_t sched_min, int32_t last_fired_day)
{
    if (!clock_synced || !door_open) return false;
    if (now_min >= 1440 || sched_min >= 1440) return false;
    unsigned since = ((unsigned)now_min + 1440u - (unsigned)sched_min) % 1440u;
    if (since >= CLOSE_SCHEDULE_WINDOW_MIN) return false;
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
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `python3 -m unittest discover -s tests -v`
Expected: PASS (21 existing + 8 new = 29 tests).

- [ ] **Step 6: Commit**

```bash
git add main/form_parse.h main/close_schedule.h tests/test_close_schedule.py
git commit -m "Add pure scheduled-close logic and form parsing"
```

---

### Task 2: Clock sync module

**Files:**
- Create: `main/clock_sync.h`
- Create: `main/clock_sync.cpp`
- Modify: `main/CMakeLists.txt` (add `"clock_sync.cpp"` to `SRCS`, after `"diag_webserver.cpp"`)
- Modify: `main/homekit.cpp:430` (call `clock_sync_start()` right after `app_wifi_init();`, and add `#include "clock_sync.h"` with the other local includes)

**Interfaces:**
- Consumes: `close_schedule_tz_valid`, `close_schedule_days_from_civil`, `CLOSE_SCHEDULE_MIN_VALID_EPOCH` from Task 1.
- Produces (all `extern "C"`):
  - `void clock_sync_start(void)`
  - `esp_err_t clock_sync_apply_timezone(const char *posix_tz)`: `ESP_ERR_INVALID_ARG` if invalid
  - `bool clock_sync_is_synced(void)`
  - `bool clock_sync_local_now(uint16_t *minute_of_day, int32_t *day_number)`: false if not synced
  - `void clock_sync_format_local(char *out, size_t out_len)`: `"YYYY-MM-DD HH:MM"` or `"unsynced"`

This module is hardware glue (SNTP, libc time); its pure parts are covered by Task 1 tests, and it is verified by the firmware build here and by the on-device check in Task 6.

- [ ] **Step 1: Write `main/clock_sync.h`**

```c
// clock_sync.h
//
// SNTP time sync plus POSIX-timezone local time helpers. The scheduler uses
// these; nothing here touches the garage door.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts SNTP (pool.ntp.org). Call once, after the default event loop and
// netif exist (i.e. right after app_wifi_init()). Sync happens when an IP is
// obtained and is renewed automatically on reconnect.
void clock_sync_start(void);

// Sets the process timezone. ESP_ERR_INVALID_ARG if the string fails
// close_schedule_tz_valid().
esp_err_t clock_sync_apply_timezone(const char *posix_tz);

// True once the wall clock is plausibly set (>= 2024-01-01 UTC).
bool clock_sync_is_synced(void);

// Local minute-of-day (0..1439) and local day number (days since 1970-01-01
// of the local date). Returns false, leaving outputs untouched, if unsynced.
bool clock_sync_local_now(uint16_t *minute_of_day, int32_t *day_number);

// "YYYY-MM-DD HH:MM" local time, or "unsynced". out_len should be >= 24.
void clock_sync_format_local(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Write `main/clock_sync.cpp`**

```cpp
#include "clock_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "close_schedule.h"

static const char *TAG = "clock";

extern "C" void clock_sync_start(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP start failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "SNTP started");
}

extern "C" esp_err_t clock_sync_apply_timezone(const char *posix_tz)
{
    if (!close_schedule_tz_valid(posix_tz)) return ESP_ERR_INVALID_ARG;
    if (setenv("TZ", posix_tz, 1) != 0) return ESP_FAIL;
    tzset();
    return ESP_OK;
}

extern "C" bool clock_sync_is_synced(void)
{
    return time(nullptr) >= CLOSE_SCHEDULE_MIN_VALID_EPOCH;
}

extern "C" bool clock_sync_local_now(uint16_t *minute_of_day, int32_t *day_number)
{
    time_t now = time(nullptr);
    if (now < CLOSE_SCHEDULE_MIN_VALID_EPOCH) return false;
    struct tm local;
    localtime_r(&now, &local);
    *minute_of_day = (uint16_t)(local.tm_hour * 60 + local.tm_min);
    *day_number = close_schedule_days_from_civil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return true;
}

extern "C" void clock_sync_format_local(char *out, size_t out_len)
{
    time_t now = time(nullptr);
    if (now < CLOSE_SCHEDULE_MIN_VALID_EPOCH) {
        snprintf(out, out_len, "unsynced");
        return;
    }
    struct tm local;
    localtime_r(&now, &local);
    strftime(out, out_len, "%Y-%m-%d %H:%M", &local);
}
```

- [ ] **Step 3: Register the source and start SNTP**

In `main/CMakeLists.txt`, add `"clock_sync.cpp"` under `SRCS` and `lwip` under `PRIV_REQUIRES` (`esp_netif_sntp.h` includes lwIP's `esp_sntp.h`). In `main/homekit.cpp`, add `#include "clock_sync.h"` next to the other local includes, and change:

```cpp
    app_wifi_init();

    hap_start();
```
to:
```cpp
    app_wifi_init();

    // SNTP needs the event loop and netif that app_wifi_init() just created.
    clock_sync_start();

    hap_start();
```

- [ ] **Step 4: Verify the firmware compiles**

Run: `idf.py build` with ESP-IDF v5.4.1 if available locally (CI uses `espressif/esp-idf-ci-action@v1` with `v5.4.1`). If the toolchain is not available, say so explicitly in the task report and rely on the CI build after pushing; do not claim it was built.
Expected: build succeeds, no warnings from `clock_sync.cpp`.

- [ ] **Step 5: Run host tests, then commit**

Run: `python3 -m unittest discover -s tests -v` → all pass.

```bash
git add main/clock_sync.h main/clock_sync.cpp main/CMakeLists.txt main/homekit.cpp
git commit -m "Add SNTP clock sync and timezone helpers"
```

---

### Task 3: Settings storage (NVS accessors)

**Files:**
- Modify: `main/gdo_settings.h` (add `#include <stdbool.h>`, `#include "close_schedule.h"`, and the two declarations inside the `extern "C"` block)
- Modify: `main/gdo-blaq-homekit.cpp`: includes near line 22 (`#include "pre_close_warning.h"`), statics near line 152, new load/get/set functions after `gdo_get_auto_close_remaining_ms()` (about line 861), call in `app_main` after `auto_close_settings_load();` (about line 1463)
- Test: `tests/test_sched_close.py` (created in Task 4; this task has no host-testable logic beyond Task 1, so it is verified by build plus on-device check)

**Interfaces:**
- Consumes: `gdo_close_schedule_t`, `close_schedule_tz_valid`, `CLOSE_SCHEDULE_*` (Task 1); `clock_sync_apply_timezone` (Task 2).
- Produces:
  - `esp_err_t gdo_get_close_schedule(gdo_close_schedule_t *out)`: `ESP_ERR_INVALID_ARG` if null
  - `esp_err_t gdo_set_close_schedule(const gdo_close_schedule_t *in)`: validates, applies the timezone, updates memory, persists, returns the NVS error if persisting fails (in-memory value stays updated, exactly like `gdo_set_auto_close_enabled()`)
  - file-static in `gdo-blaq-homekit.cpp`: `s_close_schedule` (`gdo_close_schedule_t`), `s_close_schedule_last_day` (`int32_t`), `close_schedule_last_day_save(int32_t)`

- [ ] **Step 1: Declare the accessors**

In `main/gdo_settings.h`, add after the existing includes:

```c
#include <stdbool.h>
#include "close_schedule.h"
```
and inside the `extern "C"` block, after `gdo_get_auto_close_remaining_ms`:

```c
// Scheduled close: close the door if it is open at a set local time of day.
// Defined in gdo-blaq-homekit.cpp (NVS-backed). set() validates the time
// (0..1439) and POSIX timezone string, applies the timezone immediately, and
// cancels any warning already counting down.
esp_err_t gdo_get_close_schedule(gdo_close_schedule_t *out);
esp_err_t gdo_set_close_schedule(const gdo_close_schedule_t *in);
```

- [ ] **Step 2: Add includes and state to `gdo-blaq-homekit.cpp`**

Add `#include "close_schedule.h"` and `#include "clock_sync.h"` next to `#include "pre_close_warning.h"`. After `static volatile bool s_auto_close_enabled = false;` add:

```cpp
// Scheduled close (see close_schedule.h). Guarded by ControlGuard. Off by
// default for the same reason auto-close is: the door moving on its own is a
// behavior change that must be opted into.
static gdo_close_schedule_t s_close_schedule = {
    false, CLOSE_SCHEDULE_DEFAULT_MIN, CLOSE_SCHEDULE_DEFAULT_TZ
};
// Local day number of the window that last fired; persisted so a reboot
// inside the window does not close the door a second time.
static int32_t s_close_schedule_last_day = 0;
```

- [ ] **Step 3: Add NVS keys, load, get, set, last-day save**

Place directly after `gdo_get_auto_close_remaining_ms()`:

```cpp
#define GDO_SETTINGS_NVS_KEY_SCHED_EN    "sched_en"
#define GDO_SETTINGS_NVS_KEY_SCHED_MIN   "sched_min"
#define GDO_SETTINGS_NVS_KEY_SCHED_TZ    "tz"
#define GDO_SETTINGS_NVS_KEY_SCHED_LAST  "sched_last_day"

// Called once in app_main() after NVS init. Missing keys keep the defaults.
static void close_schedule_settings_load(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(GDO_SETTINGS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        uint8_t en = 0;
        if (nvs_get_u8(handle, GDO_SETTINGS_NVS_KEY_SCHED_EN, &en) == ESP_OK) {
            s_close_schedule.enabled = (en != 0);
        }
        uint16_t minute = 0;
        if (nvs_get_u16(handle, GDO_SETTINGS_NVS_KEY_SCHED_MIN, &minute) == ESP_OK && minute < 1440) {
            s_close_schedule.minute = minute;
        }
        char tz[sizeof(s_close_schedule.tz)] = {0};
        size_t tz_len = sizeof(tz);
        if (nvs_get_str(handle, GDO_SETTINGS_NVS_KEY_SCHED_TZ, tz, &tz_len) == ESP_OK &&
            close_schedule_tz_valid(tz)) {
            strcpy(s_close_schedule.tz, tz);
        }
        int32_t last_day = 0;
        if (nvs_get_i32(handle, GDO_SETTINGS_NVS_KEY_SCHED_LAST, &last_day) == ESP_OK) {
            s_close_schedule_last_day = last_day;
        }
        nvs_close(handle);
    } else {
        ESP_LOGI(TAG, "No saved close schedule (%s) - using defaults", esp_err_to_name(err));
    }
    clock_sync_apply_timezone(s_close_schedule.tz);
    ESP_LOGI(TAG, "Close schedule: %s at %02u:%02u, tz %s",
             s_close_schedule.enabled ? "enabled" : "disabled",
             (unsigned)(s_close_schedule.minute / 60), (unsigned)(s_close_schedule.minute % 60),
             s_close_schedule.tz);
}

extern "C" esp_err_t gdo_get_close_schedule(gdo_close_schedule_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    ControlGuard guard;
    *out = s_close_schedule;
    return ESP_OK;
}

extern "C" esp_err_t gdo_set_close_schedule(const gdo_close_schedule_t *in)
{
    if (!in || in->minute >= 1440 ||
        memchr(in->tz, '\0', sizeof(in->tz)) == nullptr ||
        !close_schedule_tz_valid(in->tz)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t tz_err = clock_sync_apply_timezone(in->tz);
    if (tz_err != ESP_OK) return tz_err;
    {
        ControlGuard guard;
        ++s_close_generation;   // cancel any warning already counting down
        s_close_schedule = *in;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(GDO_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Close schedule updated in memory but failed to open NVS: %s",
                 esp_err_to_name(err));
        return err;
    }
    nvs_set_u8(handle, GDO_SETTINGS_NVS_KEY_SCHED_EN, in->enabled ? 1 : 0);
    nvs_set_u16(handle, GDO_SETTINGS_NVS_KEY_SCHED_MIN, in->minute);
    nvs_set_str(handle, GDO_SETTINGS_NVS_KEY_SCHED_TZ, in->tz);
    esp_err_t commit_err = nvs_commit(handle);
    nvs_close(handle);

    if (commit_err == ESP_OK) {
        ESP_LOGI(TAG, "Close schedule %s at %02u:%02u, tz %s - saved to NVS",
                 in->enabled ? "enabled" : "disabled",
                 (unsigned)(in->minute / 60), (unsigned)(in->minute % 60), in->tz);
    } else {
        ESP_LOGW(TAG, "Close schedule updated in memory but failed to save: %s",
                 esp_err_to_name(commit_err));
    }
    return commit_err;
}

// Persisted only when the schedule actually fires (once per day at most), to
// keep flash wear negligible.
static void close_schedule_last_day_save(int32_t day)
{
    nvs_handle_t handle;
    if (nvs_open(GDO_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "Could not open NVS to record scheduled-close day");
        return;
    }
    nvs_set_i32(handle, GDO_SETTINGS_NVS_KEY_SCHED_LAST, day);
    if (nvs_commit(handle) != ESP_OK) {
        ESP_LOGW(TAG, "Could not save scheduled-close day");
    }
    nvs_close(handle);
}
```

In `app_main`, right after `auto_close_settings_load();`:

```cpp
    close_schedule_settings_load();
```

`<string.h>` is needed for `memchr`/`strcpy`; add `#include <string.h>` to the includes if not already present.

- [ ] **Step 4: Verify build and tests**

Run: `idf.py build` if the toolchain is available (else state that it was not built; CI will build). Run `python3 -m unittest discover -s tests -v` → all pass.

- [ ] **Step 5: Commit**

```bash
git add main/gdo_settings.h main/gdo-blaq-homekit.cpp
git commit -m "Persist scheduled-close settings in NVS"
```

---

### Task 4: Scheduler trigger and warning-then-close

**Files:**
- Modify: `main/gdo-blaq-homekit.cpp`: new statics, `sched_close_warning_complete_cb()` and `sched_close_check()` immediately after `auto_close_warning_complete_cb()` (about line 1141), call inside `gdo_watchdog_task` after the auto-close block (about line 1398)
- Test: `tests/test_sched_close.py`

**Interfaces:**
- Consumes: `gdo_get_close_schedule`, `s_close_schedule_last_day`, `close_schedule_last_day_save` (Task 3); `clock_sync_local_now` (Task 2); `close_schedule_should_fire`, `close_schedule_window_day` (Task 1); existing `pre_close_warning_run_async`, `gdo_get_status`, `gdo_door_close`, `gdo_refresh_status`, `notify_homekit_target_door_state_change`, `ControlGuard`, `last_door`, `last_obstruction`, `s_close_generation`, `s_auto_close_triggered`, `s_user_close_pending`.
- Produces: `static void sched_close_check(void)` (called every watchdog tick, 5 s) and `static void sched_close_warning_complete_cb(void)`.

Behavior: fire only when `close_schedule_should_fire(...)` is true. If obstruction is not Clear, hold off without recording the day, and retry on later ticks within the window. Once the warning starts, record the window day (in memory and NVS). Skip while an auto-close or user close is already pending.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_sched_close.py`:

```python
"""Host tests for the scheduled-close trigger and its completion callback."""
import unittest
from test_regressions import ROOT, function, run_c

PRELUDE = r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include "''' + str(ROOT / 'main/close_schedule.h') + r'''"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define TAG "t"
#define ESP_OK 0
#define ESP_FAIL -1
#define GDO_DOOR_STATE_OPEN 0
#define GDO_DOOR_STATE_CLOSED 1
#define GDO_OBSTRUCTION_STATE_CLEAR 0
#define TGT_CLOSED 1
#define PRE_CLOSE_WARNING_DURATION_MS 5000
struct ControlGuard {};
struct gdo_status_t { bool synced; int door, obstruction; uint32_t door_command_id; } current;
static int last_door, last_obstruction;
static uint32_t s_close_generation;
static bool s_auto_close_triggered, s_user_close_pending;
static gdo_close_schedule_t s_close_schedule;
static int32_t s_close_schedule_last_day;
static uint32_t s_sched_close_generation, s_sched_close_command_id;
static bool s_sched_close_pending;
static bool clock_ok; static uint16_t clock_min; static int32_t clock_day;
static int warning_calls, warning_fail, saves, saved_day, refreshes, closes, notifies;
static void (*warning_cb)(void);
static int gdo_get_status(gdo_status_t *s) { *s = current; return ESP_OK; }
static int gdo_door_close() { ++closes; return ESP_OK; }
static void gdo_refresh_status() { ++refreshes; }
static void notify_homekit_target_door_state_change(int) { ++notifies; }
static int gdo_get_close_schedule(gdo_close_schedule_t *o) { *o = s_close_schedule; return ESP_OK; }
static bool clock_sync_local_now(uint16_t *m, int32_t *d) { if(!clock_ok) return false; *m=clock_min; *d=clock_day; return true; }
static int pre_close_warning_run_async(uint32_t, void (*cb)(void)) { ++warning_calls; warning_cb = cb; return warning_fail ? ESP_FAIL : ESP_OK; }
static void close_schedule_last_day_save(int32_t d) { ++saves; saved_day = d; }
'''

RESET = r'''
static void reset() {
 current = {true, GDO_DOOR_STATE_OPEN, GDO_OBSTRUCTION_STATE_CLEAR, 7};
 last_door = GDO_DOOR_STATE_OPEN; last_obstruction = GDO_OBSTRUCTION_STATE_CLEAR;
 s_close_generation = 3; s_auto_close_triggered = false; s_user_close_pending = false;
 s_close_schedule = {true, 23*60, "UTC0"}; s_close_schedule_last_day = 0;
 s_sched_close_pending = false; s_sched_close_generation = s_sched_close_command_id = 0;
 clock_ok = true; clock_min = 23*60 + 1; clock_day = 20000;
 warning_calls = warning_fail = saves = saved_day = refreshes = closes = notifies = 0; warning_cb = nullptr;
}
'''


def sources():
    cb = function('main/gdo-blaq-homekit.cpp', 'static void sched_close_warning_complete_cb(void)')
    chk = function('main/gdo-blaq-homekit.cpp', 'static void sched_close_check(void)')
    return PRELUDE + cb + '\n' + chk + '\n' + RESET


class SchedCloseTests(unittest.TestCase):
    def test_fires_once_inside_window_and_records_day(self):
        run_c(sources() + r'''
int main() {
 reset(); sched_close_check();
 assert(warning_calls==1 && saves==1 && saved_day==20000 && s_close_schedule_last_day==20000);
 assert(s_sched_close_pending && s_sched_close_command_id==7 && s_sched_close_generation==3);
 sched_close_check(); assert(warning_calls==1);          /* still pending */
 s_sched_close_pending = false; sched_close_check();
 assert(warning_calls==1 && saves==1);                   /* already fired this window */
 return 0;
}''', cpp=True)

    def test_does_not_fire_when_conditions_not_met(self):
        run_c(sources() + r'''
int main() {
 reset(); s_close_schedule.enabled=false; sched_close_check(); assert(warning_calls==0);
 reset(); clock_ok=false; sched_close_check(); assert(warning_calls==0);            /* unsynced clock */
 reset(); clock_min=22*60+59; sched_close_check(); assert(warning_calls==0);        /* before window */
 reset(); clock_min=23*60+5; sched_close_check(); assert(warning_calls==0);         /* after window */
 reset(); current.door=GDO_DOOR_STATE_CLOSED; last_door=GDO_DOOR_STATE_CLOSED;
 sched_close_check(); assert(warning_calls==0);
 reset(); current.synced=false; sched_close_check(); assert(warning_calls==0);
 reset(); s_auto_close_triggered=true; sched_close_check(); assert(warning_calls==0);
 reset(); s_user_close_pending=true; sched_close_check(); assert(warning_calls==0);
 return 0;
}''', cpp=True)

    def test_obstruction_holds_off_without_burning_the_day(self):
        run_c(sources() + r'''
int main() {
 reset(); current.obstruction=1; sched_close_check();
 assert(warning_calls==0 && refreshes==1 && saves==0 && s_close_schedule_last_day==0);
 current.obstruction=GDO_OBSTRUCTION_STATE_CLEAR; clock_min=23*60+3;   /* clears later in window */
 sched_close_check(); assert(warning_calls==1 && saves==1);
 reset(); last_obstruction=1; sched_close_check();
 assert(warning_calls==0 && refreshes==1);
 return 0;
}''', cpp=True)

    def test_warning_start_failure_retries_next_tick(self):
        run_c(sources() + r'''
int main() {
 reset(); warning_fail=1; sched_close_check();
 assert(warning_calls==1 && saves==0 && !s_sched_close_pending && s_close_schedule_last_day==0);
 warning_fail=0; sched_close_check(); assert(warning_calls==2 && saves==1);
 return 0;
}''', cpp=True)

    def test_midnight_straddling_window_fires_once(self):
        run_c(sources() + r'''
int main() {
 reset(); s_close_schedule.minute = 23*60+58; clock_min = 23*60+59; clock_day = 20000;
 sched_close_check(); assert(warning_calls==1 && saved_day==20000);
 s_sched_close_pending=false; clock_min = 1; clock_day = 20001;       /* 00:01 next day, same window */
 sched_close_check(); assert(warning_calls==1);
 return 0;
}''', cpp=True)

    def test_callback_closes_only_if_still_valid(self):
        run_c(sources() + r'''
static void arm() { reset(); s_sched_close_pending=true; s_sched_close_generation=3; s_sched_close_command_id=7; }
int main() {
 arm(); sched_close_warning_complete_cb();
 assert(closes==1 && notifies==1 && !s_sched_close_pending);
 arm(); s_close_schedule.enabled=false; sched_close_warning_complete_cb(); assert(closes==0 && !s_sched_close_pending);
 arm(); ++s_close_generation; sched_close_warning_complete_cb(); assert(closes==0 && !s_sched_close_pending); /* user/state change */
 arm(); ++current.door_command_id; sched_close_warning_complete_cb(); assert(closes==0);
 arm(); current.obstruction=1; sched_close_warning_complete_cb(); assert(closes==0);
 arm(); last_obstruction=1; sched_close_warning_complete_cb(); assert(closes==0);
 arm(); current.door=GDO_DOOR_STATE_CLOSED; sched_close_warning_complete_cb(); assert(closes==0);
 arm(); current.synced=false; sched_close_warning_complete_cb(); assert(closes==0);
 return 0;
}''', cpp=True)


if __name__ == '__main__':
    unittest.main()
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python3 -m unittest tests.test_sched_close -v` (or from `tests/`)
Expected: FAIL (`ValueError: substring not found`: the functions don't exist yet).

- [ ] **Step 3: Add the statics**

In `main/gdo-blaq-homekit.cpp`, next to `s_close_schedule_last_day` (Task 3), add:

```cpp
// Scheduled-close bookkeeping, mirrors s_auto_close_generation/command_id:
// the warning only leads to a close if nothing changed while it ran.
static uint32_t s_sched_close_generation;
static uint32_t s_sched_close_command_id;
static bool s_sched_close_pending;
```

- [ ] **Step 4: Implement the callback and check**

Place after `auto_close_warning_complete_cb()`:

```cpp
// Fires once the scheduled-close warning finishes. Revalidates everything,
// exactly like auto_close_warning_complete_cb(): any command, state change or
// settings change during the warning bumps s_close_generation and cancels it.
static void sched_close_warning_complete_cb(void)
{
    ControlGuard guard;
    gdo_status_t status;
    bool valid = gdo_get_status(&status) == ESP_OK && status.synced &&
        status.door == GDO_DOOR_STATE_OPEN && status.obstruction == GDO_OBSTRUCTION_STATE_CLEAR &&
        last_obstruction == GDO_OBSTRUCTION_STATE_CLEAR && s_close_schedule.enabled &&
        s_sched_close_generation == s_close_generation &&
        s_sched_close_command_id == status.door_command_id;
    s_sched_close_pending = false;
    if (!valid) {
        ESP_LOGI(TAG, "Scheduled close cancelled during warning");
        return;
    }
    if (gdo_door_close() == ESP_OK) notify_homekit_target_door_state_change(TGT_CLOSED);
    else ESP_LOGW(TAG, "Scheduled close: close command failed");
}

// Called every watchdog tick. Cheap when disabled or outside the window.
static void sched_close_check(void)
{
    gdo_close_schedule_t sched;
    gdo_get_close_schedule(&sched);
    if (!sched.enabled) return;

    uint16_t now_min;
    int32_t today;
    bool synced = clock_sync_local_now(&now_min, &today);
    if (!synced) return;   // never act on an unset clock

    ControlGuard guard;
    if (s_sched_close_pending || s_auto_close_triggered || s_user_close_pending) return;

    gdo_status_t current;
    if (gdo_get_status(&current) != ESP_OK || !current.synced) return;
    bool door_open = current.door == GDO_DOOR_STATE_OPEN && last_door == GDO_DOOR_STATE_OPEN;
    if (!close_schedule_should_fire(synced, door_open, now_min, today, sched.minute,
                                    s_close_schedule_last_day)) {
        return;
    }

    if (current.obstruction != GDO_OBSTRUCTION_STATE_CLEAR ||
        last_obstruction != GDO_OBSTRUCTION_STATE_CLEAR) {
        // Same stance as auto-close: err toward not closing. Don't record the
        // day, so a later tick inside the window can still fire.
        ESP_LOGW(TAG, "Scheduled close: obstruction not Clear - holding off");
        gdo_refresh_status();
        return;
    }

    int32_t window_day = close_schedule_window_day(now_min, today, sched.minute);
    ESP_LOGW(TAG, "Scheduled close: door open at scheduled time - sounding warning and closing");
    s_sched_close_generation = s_close_generation;
    s_sched_close_command_id = current.door_command_id;
    s_sched_close_pending = true;
    if (pre_close_warning_run_async(PRE_CLOSE_WARNING_DURATION_MS,
                                    sched_close_warning_complete_cb) != ESP_OK) {
        s_sched_close_pending = false;
        ESP_LOGE(TAG, "Scheduled close cancelled: warning could not start");
        return;   // not recorded; retried next tick while the window is open
    }
    s_close_schedule_last_day = window_day;
    close_schedule_last_day_save(window_day);
}
```

- [ ] **Step 5: Hook into the watchdog**

In `gdo_watchdog_task`, immediately after the closing brace of the `// 3) Auto-close` `if` block (the block that ends just before the loop's closing brace), add:

```cpp
        // 4) Scheduled close: see sched_close_check().
        sched_close_check();
```
Take care that the auto-close block's `continue` statement (inside its `if (gdo_get_status(...)...) continue;`) skips this on that tick, which is fine.

- [ ] **Step 6: Run tests to verify they pass**

Run: `python3 -m unittest discover -s tests -v`
Expected: PASS (all earlier tests plus 6 new).

- [ ] **Step 7: Verify the firmware compiles, then commit**

Run `idf.py build` if available (otherwise state it wasn't built).

```bash
git add main/gdo-blaq-homekit.cpp tests/test_sched_close.py
git commit -m "Close the garage door at the scheduled time if it is open"
```

---

### Task 5: Web endpoint, status fields, and settings UI

**Files:**
- Modify: `main/diag_webserver.cpp`: includes (~line 22-40), POST handler after `auto_close_settings_post_handler` (~line 527), `/status` handler (~lines 1030-1125), UI card after the Auto-Close card (~line 703), JS functions after `saveAutoClose` (~line 903), URI struct and registration (~lines 1221-1285)
- Modify: `main/diag_webserver.cpp` CSS (~line 607)
- Test: `tests/test_close_schedule.py` (add handler-level test), existing dashboard JS test in `tests/test_log_security.py` must keep passing

**Interfaces:**
- Consumes: `gdo_get_close_schedule`, `gdo_set_close_schedule` (Task 3); `close_schedule_parse_form`, `CLOSE_SCHEDULE_FORM_*` (Task 1); `clock_sync_is_synced`, `clock_sync_format_local` (Task 2); existing `authorize_mutation`.
- Produces: `POST /settings/close-schedule` (form body `enabled=1&time=HH%3AMM&tz=<urlencoded>`; 200 `OK`, 400 on bad time or timezone, 401/403 from `authorize_mutation`, 500 on set failure); `/status` JSON gains `close_schedule_enabled` (bool), `close_schedule_time` ("HH:MM"), `timezone` (string), `clock_synced` (bool), `local_time` (string).

Note on UI JavaScript: `tests/test_log_security.py::test_dashboard_log_fetch_and_download_send_token_headers` executes the embedded page script under Node with a stub `document`. Keep new JS inside functions (no top-level DOM access), and keep the `static const char *page` literal intact.

- [ ] **Step 1: Write the failing test for the handler**

Append to `tests/test_close_schedule.py` (inside a new class, plus imports `function`):

```python
from test_regressions import function


class CloseScheduleHandlerTests(unittest.TestCase):
    def test_post_handler_validates_then_saves(self):
        fn = function('main/diag_webserver.cpp', 'static esp_err_t close_schedule_post_handler(')
        run_c(r'''
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "''' + str(ROOT / 'main/close_schedule.h') + r'''"
#define ESP_OK 0
#define ESP_FAIL -1
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_RESP_USE_STRLEN -1
#define ESP_LOGI(...) ((void)0)
#define TAG "t"
typedef int esp_err_t;
struct httpd_req_t { const char *body; size_t content_len; size_t pos; int status; bool ok_sent; };
static bool auth_ok = true;
static int sets, set_result;
static gdo_close_schedule_t last_set;
static bool authorize_mutation(httpd_req_t *) { return auth_ok; }
static int httpd_req_recv(httpd_req_t *r, char *buf, size_t n) {
 size_t left = r->content_len - r->pos; if (n > left) n = left;
 memcpy(buf, r->body + r->pos, n); r->pos += n; return (int)n; }
static void httpd_resp_send_err(httpd_req_t *r, int s, const char *) { r->status = s; }
static void httpd_resp_send_500(httpd_req_t *r) { r->status = 500; }
static void httpd_resp_set_type(httpd_req_t *, const char *) {}
static int httpd_resp_send(httpd_req_t *r, const char *, int) { r->ok_sent = true; r->status = 200; return ESP_OK; }
static int gdo_set_close_schedule(const gdo_close_schedule_t *s) { ++sets; last_set = *s; return set_result; }
''' + fn + r'''
static int post(const char *body) {
 httpd_req_t r = {body, strlen(body), 0, 0, false};
 close_schedule_post_handler(&r); return r.status; }
int main() {
 assert(post("enabled=1&time=23%3A00&tz=PST8PDT%2CM3.2.0%2CM11.1.0")==200);
 assert(sets==1 && last_set.enabled && last_set.minute==1380 && !strcmp(last_set.tz,"PST8PDT,M3.2.0,M11.1.0"));
 assert(post("enabled=1&time=99%3A00&tz=UTC0")==400 && sets==1);        /* bad time: nothing saved */
 assert(post("enabled=1&time=23%3A00&tz=bad%20tz")==400 && sets==1);    /* bad tz: nothing saved */
 assert(post("")==400 && sets==1);                                       /* empty body */
 char big[600]; memset(big,'a',599); big[599]=0;
 assert(post(big)==400 && sets==1);                                      /* oversized body */
 auth_ok=false; assert(post("enabled=1&time=23%3A00&tz=UTC0")==0 && sets==1); /* unauthorized: no response sent by us, no save */
 auth_ok=true; set_result=-1; assert(post("enabled=1&time=23%3A00&tz=UTC0")==500);
 return 0;
}''', cpp=True)
```

- [ ] **Step 2: Run to verify failure**

Run: `python3 -m unittest tests.test_close_schedule.CloseScheduleHandlerTests -v`
Expected: FAIL (`close_schedule_post_handler` not found).

- [ ] **Step 3: Add includes and the POST handler**

In `diag_webserver.cpp` add `#include "close_schedule.h"` and `#include "clock_sync.h"` after `#include "gdo_settings.h"`. Add after `auto_close_settings_post_handler`:

```cpp
// Body: application/x-www-form-urlencoded, e.g.
// "enabled=1&time=23%3A00&tz=PST8PDT%2CM3.2.0%2CM11.1.0". The timezone can
// contain characters the browser percent-encodes, so this one does use the
// decoding parser in form_parse.h (unlike the auto-close handler above).
static esp_err_t close_schedule_post_handler(httpd_req_t *req)
{
    if (!authorize_mutation(req)) return ESP_FAIL;
    char buf[256] = {0};
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request body");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < len) {
        int n = httpd_req_recv(req, buf + received, len - received);
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
            return ESP_FAIL;
        }
        received += n;
    }
    buf[received] = '\0';

    gdo_close_schedule_t sched;
    switch (close_schedule_parse_form(buf, &sched)) {
    case CLOSE_SCHEDULE_FORM_OK:
        break;
    case CLOSE_SCHEDULE_FORM_BAD_TIME:
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Time must be HH:MM (00:00-23:59)");
        return ESP_FAIL;
    default:
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid timezone string");
        return ESP_FAIL;
    }

    if (gdo_set_close_schedule(&sched) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Close schedule updated via web: enabled=%s, %02u:%02u, tz=%s",
             sched.enabled ? "true" : "false",
             (unsigned)(sched.minute / 60), (unsigned)(sched.minute % 60), sched.tz);

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}
```

- [ ] **Step 4: Register the route**

Next to `auto_close_uri`:

```cpp
    httpd_uri_t close_schedule_uri = {};
    close_schedule_uri.uri = "/settings/close-schedule";
    close_schedule_uri.method = HTTP_POST;
    close_schedule_uri.handler = close_schedule_post_handler;
```
and after the `auto_close_uri` registration block:

```cpp
    if (httpd_register_uri_handler(s_server, &close_schedule_uri) != ESP_OK) {
        httpd_stop(s_server);
        s_server = nullptr;
        return false;
    }
```
Check `HTTPD_DEFAULT_CONFIG()` / `config.max_uri_handlers` (default 8) against the number of registered handlers; if registering this one would exceed it, raise `config.max_uri_handlers` accordingly (count the `httpd_register_uri_handler` calls in `diag_webserver_start`).

- [ ] **Step 5: Extend `/status`**

Before the `char buf[1088];` line, add:

```cpp
    gdo_close_schedule_t sched;
    gdo_get_close_schedule(&sched);
    char local_time[24];
    clock_sync_format_local(local_time, sizeof(local_time));
```
Change `char buf[1088];` to `char buf[1344];`. In the format string, after `"\"auto_close_remaining_s\":%lld,"` add:

```
        "\"close_schedule_enabled\":%s,"
        "\"close_schedule_time\":\"%02u:%02u\","
        "\"timezone\":\"%s\","
        "\"clock_synced\":%s,"
        "\"local_time\":\"%s\","
```
and after the matching `auto_close_remaining_s,` argument add:

```cpp
        sched.enabled ? "true" : "false",
        (unsigned)(sched.minute / 60), (unsigned)(sched.minute % 60),
        sched.tz,
        clock_sync_is_synced() ? "true" : "false",
        local_time,
```
The timezone string is safe to embed unescaped because `close_schedule_tz_valid()` excludes quotes and backslashes and both write paths validate.

- [ ] **Step 6: Add the UI card, CSS, and JS**

CSS (extend the existing rules near line 607): add `input[type="time"],input[type="text"]` to the selector list that styles `input[type="number"]`, and add `input[type="text"]{width:190px;}`.

Card, inserted after the Auto-Close card's closing `"  </div>"`:

```
        "  <div class=\"card\">"
        "    <h2>Scheduled Close</h2>"
        "    <div class=\"row\">"
        "      <div class=\"label\"><span class=\"ic\">&#128337;</span>Enabled</div>"
        "      <label class=\"switch\"><input type=\"checkbox\" id=\"scEnabled\"><span class=\"track\"></span></label>"
        "    </div>"
        "    <div class=\"row\">"
        "      <div class=\"label\"><span class=\"ic\">&#9200;</span>Close at</div>"
        "      <span class=\"value mono\"><input type=\"time\" id=\"scTime\" value=\"23:00\"></span>"
        "    </div>"
        "    <div class=\"row\">"
        "      <div class=\"label\"><span class=\"ic\">&#127758;</span>Timezone</div>"
        "      <span class=\"value mono\"><input type=\"text\" id=\"scTz\" maxlength=\"63\" placeholder=\"PST8PDT,M3.2.0,M11.1.0\"></span>"
        "    </div>"
        "    <div class=\"divider\"></div>"
        "    <div class=\"btnrow\">"
        "      <button class=\"primary\" onclick=\"saveSchedule()\">Save</button>"
        "      <button onclick=\"useBrowserTz()\">Use browser timezone</button>"
        "      <span id=\"scStatus\" class=\"hint\"></span>"
        "    </div>"
        "    <div id=\"scInfo\" class=\"hint\"></div>"
        "  </div>"
```

JS, inserted after `saveAutoClose()`:

```
        "let scLoaded = false;"
        "const TZ_MAP = {"
        "  'America/New_York':'EST5EDT,M3.2.0,M11.1.0','America/Chicago':'CST6CDT,M3.2.0,M11.1.0',"
        "  'America/Denver':'MST7MDT,M3.2.0,M11.1.0','America/Phoenix':'MST7',"
        "  'America/Los_Angeles':'PST8PDT,M3.2.0,M11.1.0','America/Anchorage':'AKST9AKDT,M3.2.0,M11.1.0',"
        "  'Pacific/Honolulu':'HST10','America/Toronto':'EST5EDT,M3.2.0,M11.1.0',"
        "  'America/Vancouver':'PST8PDT,M3.2.0,M11.1.0','Europe/London':'GMT0BST,M3.5.0/1,M10.5.0',"
        "  'Europe/Dublin':'IST-1GMT0,M10.5.0,M3.5.0/1','Europe/Paris':'CET-1CEST,M3.5.0,M10.5.0/3',"
        "  'Europe/Berlin':'CET-1CEST,M3.5.0,M10.5.0/3','Europe/Madrid':'CET-1CEST,M3.5.0,M10.5.0/3',"
        "  'Europe/Rome':'CET-1CEST,M3.5.0,M10.5.0/3','Europe/Amsterdam':'CET-1CEST,M3.5.0,M10.5.0/3',"
        "  'Europe/Athens':'EET-2EEST,M3.5.0/3,M10.5.0/4','Australia/Sydney':'AEST-10AEDT,M10.1.0,M4.1.0/3',"
        "  'Australia/Perth':'AWST-8','Pacific/Auckland':'NZST-12NZDT,M9.5.0,M4.1.0/3','UTC':'UTC0'"
        "};"
        "function useBrowserTz(){"
        "  const el = document.getElementById('scTz');"
        "  const st = document.getElementById('scStatus');"
        "  let name = '';"
        "  try { name = Intl.DateTimeFormat().resolvedOptions().timeZone || ''; } catch(e) {}"
        "  if (TZ_MAP[name]) { el.value = TZ_MAP[name]; st.textContent = 'Using ' + name + '. Save to apply.'; }"
        "  else { st.textContent = 'Unknown zone' + (name ? ' (' + name + ')' : '') + ' - enter a POSIX TZ string.'; }"
        "}"
        "function renderScheduleInfo(s){"
        "  const el = document.getElementById('scInfo');"
        "  if (!s) { el.textContent = ''; return; }"
        "  el.textContent = s.clock_synced ? ('Device local time: ' + s.local_time)"
        "                                  : 'Clock not synced yet - the schedule will not run until it is.';"
        "}"
        "async function saveSchedule(){"
        "  const enabled = document.getElementById('scEnabled').checked ? 1 : 0;"
        "  const time = document.getElementById('scTime').value;"
        "  const tz = document.getElementById('scTz').value.trim();"
        "  const statusEl = document.getElementById('scStatus');"
        "  statusEl.textContent = 'Saving...';"
        "  try{"
        "    const r = await fetch('/settings/close-schedule', {"
        "      method: 'POST',"
        "      headers: {...await authHeaders(), 'Content-Type': 'application/x-www-form-urlencoded'},"
        "      body: 'enabled=' + enabled + '&time=' + encodeURIComponent(time) + '&tz=' + encodeURIComponent(tz)"
        "    });"
        "    if(!r.ok){ if(r.status===401) adminToken=''; throw new Error(await r.text()); }"
        "    statusEl.textContent = 'Saved.';"
        "    refresh();"
        "    setTimeout(function(){ statusEl.textContent = ''; }, 3000);"
        "  }catch(e){"
        "    statusEl.textContent = 'Save failed: ' + e.message;"
        "  }"
        "}"
```

In the existing `refresh()` status handling, after `renderCountdown();` add:

```
        "    if (!scLoaded) {"
        "      document.getElementById('scEnabled').checked = !!s.close_schedule_enabled;"
        "      document.getElementById('scTime').value = s.close_schedule_time || '23:00';"
        "      document.getElementById('scTz').value = s.timezone || 'UTC0';"
        "      scLoaded = true;"
        "    }"
        "    renderScheduleInfo(s);"
```
Ordering caveat: `let scLoaded` is declared later in the script than `refresh()` is defined but before `refresh();` is first called at the bottom, so there is no temporal-dead-zone problem; it follows the same pattern as `acLoaded`. Keep it that way.

- [ ] **Step 7: Run the full suite**

Run: `python3 -m unittest discover -s tests -v`
Expected: PASS (including `test_dashboard_log_fetch_and_download_send_token_headers`, which executes the page script under Node). If that test fails with a ReferenceError on one of the new elements or functions, the new JS touches the DOM at load time; move it inside a function.

- [ ] **Step 8: Verify the firmware compiles, then commit**

Run `idf.py build` if available (otherwise state it wasn't built).

```bash
git add main/diag_webserver.cpp tests/test_close_schedule.py
git commit -m "Add scheduled-close settings to the diagnostics page"
```

---

### Task 6: Documentation and on-device verification

**Files:**
- Modify: `README.md` (add a short "Scheduled close" section next to the existing auto-close documentation, or after "Provision WiFi and add to HomeKit" if auto-close isn't documented there)

- [ ] **Step 1: Document the feature**

Find where auto-close is described (`grep -n -i "auto-close" README.md`). Add a section beside it:

```markdown
### Scheduled close

On the diagnostics page, **Scheduled Close** closes the garage door at a set time of day if it is open, for example 23:00 every night. It uses the same beep-and-flash warning as Auto-Close.

- The device gets the time over Wi-Fi (`pool.ntp.org`). Until it has synced, the schedule does nothing, and the card says so.
- Pick your timezone with **Use browser timezone**, or enter a POSIX timezone string such as `PST8PDT,M3.2.0,M11.1.0`. Daylight saving is handled automatically.
- The close runs at most once per night. If the door is open any time in the five minutes after the set time, it closes. If it was closed at that time, nothing happens until the next night.
- It is skipped (and retried for the rest of the five minutes) if the door's obstruction sensor is not clear.
- Off by default.
```

- [ ] **Step 2: Run the full suite**

Run: `python3 -m unittest discover -s tests -v` → all pass.

- [ ] **Step 3: On-device check (requires hardware; report honestly if skipped)**

1. Flash the build. Open the diagnostics page: the Scheduled Close card shows the device local time once SNTP syncs (`clock_synced` true in `/status`).
2. Use **Use browser timezone**, set the time two minutes ahead, enable, Save. Open the door. At the set time the buzzer/LED warning runs for 5 s and the door closes; the log shows `Scheduled close: door open at scheduled time`.
3. Reopen the door inside the same 5-minute window: it must NOT close again.
4. Reboot inside the window after a fire: it must NOT close again.
5. Set the time ahead with the door closed: nothing happens.

If no hardware is available, say so and list these as unverified.

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "Document scheduled close"
```
