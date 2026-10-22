#include "activity_history.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"

static portMUX_TYPE s_activity_lock = portMUX_INITIALIZER_UNLOCKED;
static activity_entry_t s_activity[ACTIVITY_HISTORY_CAPACITY];
static size_t s_activity_next;
static size_t s_activity_count;
static uint32_t s_activity_sequence;

extern "C" void activity_record(const char *source, const char *message)
{
    if (!source || !message || !*message) return;
    activity_entry_t entry = {};
    entry.uptime_ms = esp_timer_get_time() / 1000;
    time_t wall = time(nullptr);
    entry.unix_time = wall >= 1704067200 ? (int64_t)wall : 0;
    snprintf(entry.source, sizeof(entry.source), "%s", source);
    snprintf(entry.message, sizeof(entry.message), "%s", message);
    portENTER_CRITICAL(&s_activity_lock);
    // Repeated watchdog skips must not push useful events out of the ring.
    size_t previous = (s_activity_next + ACTIVITY_HISTORY_CAPACITY - 1) % ACTIVITY_HISTORY_CAPACITY;
    if (s_activity_count && !strcmp(s_activity[previous].source, entry.source) &&
        !strcmp(s_activity[previous].message, entry.message)) {
        portEXIT_CRITICAL(&s_activity_lock);
        return;
    }
    entry.id = ++s_activity_sequence;
    s_activity[s_activity_next] = entry;
    s_activity_next = (s_activity_next + 1) % ACTIVITY_HISTORY_CAPACITY;
    if (s_activity_count < ACTIVITY_HISTORY_CAPACITY) ++s_activity_count;
    portEXIT_CRITICAL(&s_activity_lock);
}

extern "C" size_t activity_read(activity_entry_t *out, size_t capacity)
{
    if (!out || !capacity) return 0;
    portENTER_CRITICAL(&s_activity_lock);
    size_t count = capacity < s_activity_count ? capacity : s_activity_count;
    size_t start = (s_activity_next + ACTIVITY_HISTORY_CAPACITY - count) % ACTIVITY_HISTORY_CAPACITY;
    for (size_t i = 0; i < count; ++i)
        out[i] = s_activity[(start + i) % ACTIVITY_HISTORY_CAPACITY];
    portEXIT_CRITICAL(&s_activity_lock);
    return count;
}
