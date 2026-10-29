#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ACTIVITY_HISTORY_CAPACITY 32
// RAM-only history. unix_time is zero until wall-clock time is available.
typedef struct {
    uint32_t id;
    int64_t uptime_ms;
    int64_t unix_time;
    char source[24];
    char message[128];
} activity_entry_t;
void activity_record(const char *source, const char *message);
// Copies the newest entries in chronological order into caller-owned memory.
size_t activity_read(activity_entry_t *out, size_t capacity);
#ifdef __cplusplus
}
#endif
