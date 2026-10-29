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
