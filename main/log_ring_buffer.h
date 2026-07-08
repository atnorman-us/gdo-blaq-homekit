#ifndef LOG_RING_BUFFER_H
#define LOG_RING_BUFFER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Hooks esp_log's vprintf so every log line (from any component, not just
// this app) is captured into a fixed-size in-memory circular buffer, in
// addition to still being printed to the original destination (UART/serial
// monitor) exactly as before. Call once, early in app_main().
//
// This is intentionally in-memory only - it does not survive a reboot.
// Persisting logs across reboots would need a filesystem partition, which
// this project's partition table doesn't currently have.
void log_ring_buffer_init(size_t capacity_bytes);

// Copies the most recent min(capacity_used, out_size) bytes of log history
// into `out`, in chronological order (oldest of the returned window first).
// Returns the number of bytes actually written to `out`.
size_t log_ring_buffer_read(char *out, size_t out_size);

// Total capacity of the ring buffer, in bytes.
size_t log_ring_buffer_capacity(void);

// How many bytes of real log history are currently held (<= capacity).
size_t log_ring_buffer_used(void);

#ifdef __cplusplus
}
#endif

#endif // LOG_RING_BUFFER_H
