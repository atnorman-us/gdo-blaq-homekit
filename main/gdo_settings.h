// gdo_settings.h
//
// Auto-close settings accessors - defined in gdo-blaq-homekit.cpp, which
// owns the NVS persistence and defaults (disabled by default, 60-minute
// default once enabled). Shared here so callers (currently
// diag_webserver.cpp) don't maintain their own separate copy of these
// declarations, which could silently drift from the real definitions.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "close_schedule.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gdo_control_open(void);
esp_err_t gdo_control_close(void);

bool     gdo_get_auto_close_enabled(void);
esp_err_t gdo_set_auto_close_enabled(bool enabled);

uint32_t gdo_get_auto_close_timeout_ms(void);

// Pre-close warning buzzer (the 5 s beep before a firmware-initiated close).
// On by default. Off keeps the LED flash and the warning period.
bool     gdo_get_pre_close_buzzer(void);
esp_err_t gdo_set_pre_close_buzzer(bool enabled);
esp_err_t gdo_set_auto_close_timeout_ms(uint32_t timeout_ms);

// Milliseconds remaining until auto-close would fire, or -1 if not
// currently counting down (disabled, door isn't confirmed open, or the
// warning/close sequence has already started for this open-cycle).
int64_t gdo_get_auto_close_remaining_ms(void);

// Scheduled close: close the door if it is open at a set local time of day.
// Defined in gdo-blaq-homekit.cpp (NVS-backed). set() validates the time
// (0..1439) and POSIX timezone string, applies the timezone immediately, and
// cancels any warning already counting down.
esp_err_t gdo_get_close_schedule(gdo_close_schedule_t *out);
esp_err_t gdo_set_close_schedule(const gdo_close_schedule_t *in);

#ifdef __cplusplus
}
#endif