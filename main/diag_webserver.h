#ifndef DIAG_WEBSERVER_H
#define DIAG_WEBSERVER_H

#ifdef __cplusplus
extern "C" {
#endif

// Starts the diagnostics web server: a small status/log page plus
// /status (JSON) and /logs, /logs/download (plain text) endpoints.
// Safe to call before WiFi is connected - the server just won't be
// reachable until an IP address is assigned.
//
// No authentication - reachable by anything on the same network as the
// HomeKit accessory itself. Add basic auth in the request handlers below
// if that's ever a concern for your network.
void diag_webserver_start(void);

// Stops and recreates the httpd server. Call this after a WiFi reconnect -
// the underlying socket doesn't reliably survive the interface bouncing.
// Safe to call even if the server was never started (no-op in that case).
void diag_webserver_restart(void);

#ifdef __cplusplus
}
#endif

#endif // DIAG_WEBSERVER_H