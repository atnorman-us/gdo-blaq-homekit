// gdo_settings.h
//
// Auto-close settings accessors - defined in gdo-blaq-homekit.cpp, which
// owns the NVS persistence and defaults (disabled by default, 60-minute
// default once enabled). Shared here so callers (currently
// diag_webserver.cpp) don't maintain their own separate copy of these
// declarations, which could silently drift from the real definitions.

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

bool     gdo_get_auto_close_enabled(void);
esp_err_t gdo_set_auto_close_enabled(bool enabled);

uint32_t gdo_get_auto_close_timeout_ms(void);
esp_err_t gdo_set_auto_close_timeout_ms(uint32_t timeout_ms);

// Milliseconds remaining until auto-close would fire, or -1 if not
// currently counting down (disabled, door isn't confirmed open, or the
// warning/close sequence has already started for this open-cycle).
int64_t gdo_get_auto_close_remaining_ms(void);

#ifdef __cplusplus
}
#endif