// diagnostics_counters.c

#include "diagnostics_counters.h"

#include "esp_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define RX_SIG_ERROR_NEEDLE       "RX data signature error"
#define LONG_OBSTRUCTION_NEEDLE   "Long duration obstruction detected"

static vprintf_like_t s_original_vprintf = NULL;
static volatile uint32_t s_rx_signature_error_count = 0;
static volatile uint32_t s_long_obstruction_count    = 0;

static int diag_counting_vprintf(const char *fmt, va_list args)
{
    // Match directly against the format string itself, not the fully
    // formatted message - both needles are LITERAL text in gdolib's own
    // ESP_LOG calls (the %02x placeholders only cover the substituted hex
    // digits, not the matched text), so there's no need to actually
    // format anything to detect them. Confirmed the hard way: an earlier
    // version of this hook formatted every log line into a 256-byte stack
    // buffer via vsnprintf(), which runs on WHATEVER task happens to be
    // logging - including tiny built-in ESP-IDF system tasks like
    // "sys_evt" with very little stack margin to spare. That caused a
    // reproducible stack overflow crash, 100% of the time, right at the
    // handful of log lines that fire in a burst when WiFi finishes
    // connecting. This version adds no stack buffer at all.
    if (fmt) {
        if (strstr(fmt, RX_SIG_ERROR_NEEDLE) != NULL) {
            s_rx_signature_error_count++;
        } else if (strstr(fmt, LONG_OBSTRUCTION_NEEDLE) != NULL) {
            s_long_obstruction_count++;
        }
    }

    // Always chain through so normal logging behavior (including this
    // line itself) is unaffected - this is purely an observer.
    if (s_original_vprintf) {
        return s_original_vprintf(fmt, args);
    }
    return vprintf(fmt, args);
}

void diagnostics_counters_init(void)
{
    s_original_vprintf = esp_log_set_vprintf(diag_counting_vprintf);
}

uint32_t diag_rx_error_count_get_and_reset(void)
{
    uint32_t count = s_rx_signature_error_count;
    s_rx_signature_error_count = 0;
    return count;
}

uint32_t diag_rx_error_count_peek(void)
{
    return s_rx_signature_error_count;
}

uint32_t diag_long_obstruction_count(void)
{
    return s_long_obstruction_count;
}