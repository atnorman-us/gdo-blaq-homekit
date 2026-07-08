#include "log_ring_buffer.h"

#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static char *s_buf = nullptr;
static size_t s_capacity = 0;
static size_t s_write_pos = 0;
static bool s_wrapped = false;
static SemaphoreHandle_t s_mutex = nullptr;
static vprintf_like_t s_original_vprintf = nullptr;

// Replaces esp_log's output function. Formats each log call into a bounded
// stack buffer, appends it into the circular buffer, then forwards to
// whatever vprintf esp_log was using before (normally UART) so existing
// serial logging behavior is unchanged.
static int ring_vprintf(const char *fmt, va_list args)
{
    char line[256];
    va_list args_copy;
    va_copy(args_copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, args_copy);
    va_end(args_copy);

    if (n > 0 && s_buf) {
        size_t len = (size_t)n;
        if (len > sizeof(line) - 1) {
            len = sizeof(line) - 1; // vsnprintf truncated; only copy what we actually have
        }

        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            for (size_t i = 0; i < len; i++) {
                s_buf[s_write_pos] = line[i];
                s_write_pos++;
                if (s_write_pos >= s_capacity) {
                    s_write_pos = 0;
                    s_wrapped = true;
                }
            }
            xSemaphoreGive(s_mutex);
        }
    }

    if (s_original_vprintf) {
        return s_original_vprintf(fmt, args);
    }
    return n;
}

void log_ring_buffer_init(size_t capacity_bytes)
{
    if (s_buf) {
        return; // already initialized
    }

    s_buf = (char *)malloc(capacity_bytes);
    s_mutex = xSemaphoreCreateMutex();

    if (!s_buf || !s_mutex) {
        // Deliberately not using ESP_LOGE here - we may be mid-initialization
        // of the very logging path we're trying to set up. printf is safe.
        printf("log_ring_buffer_init: failed to allocate %u byte buffer\n",
               (unsigned)capacity_bytes);
        free(s_buf);
        s_buf = nullptr;
        return;
    }

    s_capacity = capacity_bytes;
    s_original_vprintf = esp_log_set_vprintf(&ring_vprintf);
}

size_t log_ring_buffer_capacity(void)
{
    return s_capacity;
}

size_t log_ring_buffer_used(void)
{
    if (!s_buf) {
        return 0;
    }
    size_t used = 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        used = s_wrapped ? s_capacity : s_write_pos;
        xSemaphoreGive(s_mutex);
    }
    return used;
}

size_t log_ring_buffer_read(char *out, size_t out_size)
{
    if (!s_buf || out_size == 0) {
        return 0;
    }

    size_t copied = 0;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        size_t used = s_wrapped ? s_capacity : s_write_pos;
        size_t want = used < out_size ? used : out_size;
        size_t skip = used - want; // keep only the most recent `want` bytes
        size_t chrono_start = s_wrapped ? s_write_pos : 0;

        for (size_t i = 0; i < want; i++) {
            size_t idx = (chrono_start + skip + i) % s_capacity;
            out[i] = s_buf[idx];
        }
        copied = want;
        xSemaphoreGive(s_mutex);
    }
    return copied;
}
