// diagnostics_counters.h
//
// Hooks ESP-IDF's log vprintf to count occurrences of specific gdolib log
// lines, without needing any gdolib source changes - useful for signals
// gdolib logs internally but doesn't expose as a GDO_CB_EVENT_* callback
// (e.g. "Long duration obstruction detected"), and for the RX signature
// error count used in the per-door-cycle summary log line.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call once, as early as possible in app_main() - ideally before
// gdo_init()/gdo_start() so even boot-time messages are counted.
void diagnostics_counters_init(void);

// RX signature errors: reset at the start of each door transition and
// read back at completion, for the per-cycle summary log line.
uint32_t diag_rx_error_count_get_and_reset(void);

// Same counter, read-only - for the diagnostics dashboard to show a live
// value without disturbing the per-cycle reset tracking above.
uint32_t diag_rx_error_count_peek(void);

// Long-duration obstruction detections: lifetime count since boot, for
// the diagnostics dashboard - a real signal worth tracking over time to
// spot a sensor that trips this unusually often.
uint32_t diag_long_obstruction_count(void);

#ifdef __cplusplus
}
#endif