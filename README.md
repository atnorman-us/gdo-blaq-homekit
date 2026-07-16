# HomeKit Firmware for the GDO blaQ
 This is a HomeKit native firmware for Konnected's GDO blaQ, a smart garage door opener control accessory for Chamberlain/LiftMaster/Craftsman/Merlin garage openers with Security+ or Security+2.0.
  
 ## Submodules
 
 This repository uses git submodules. After cloning, be sure to initialize and update them:
 
 ```sh
 git submodule update --init --recursive
 ```

## Provision WiFi and Add Accessory to HomeKit

 This firmware uses the `nvs_wifi_connect` component to manage WiFi connections. The relevant code can be found in `main/wifi.cpp`.
 
 The `nvs_wifi_connect` component will start an HTTP server for configuration if the device is not connected to a WiFi network. Connect to the access point created by the device with SSID `konnected-blaq-hk` and open a web browser to `http://192.168.4.1` and enter your WiFi credentials, then click `Write and Reboot`.

 After connecting to WiFi and restarting wait about 10 seconds then open the Home app on your iOS device to add the accessory. Go to "Add Accessory" and then "more options..." to find the accessory on the network. Click on the found accessory and enter the setup code `251-02-023` when prompted and follow the instructions to complete the setup.

## Accessing the diagnostics web server
Find the device's IP — same one your router/Home app already knows it by (or check the boot log for esp_netif_handlers: sta ip: ...).
Open in a browser, on the same network as the device:

URLDashboard(
status + live log, auto-refreshing) --> `http://<device-ip>:8080/`

Raw status as JSON --> `http://<device-ip>:8080/status`

Log buffer, plain text (view/copy) --> `http://<device-ip>:8080/logs`

Log buffer, forced download --> `http://<device-ip>:8080/logs/download` → saves gdo-log.txt

## On the dashboard (/):
Status table: door / light / lock / obstruction / motion state, uptime, free heap, WiFi RSSI, connected HomeKit sessions, last reset reason, current log buffer usage.

Log pane: shows the last ~64KB of log history, refreshes every 3s by default (toggle off with the checkbox if you want to read a specific moment without it jumping).

Download full log button — same as hitting /logs/download directly, gives you a .txt file

## Pre-Close Warning:
GDO blaQ light will flash and speaker will beep for 5 seconds if closed from app.

## Reliability Fixes & New Features

The following were added on top of the base firmware to fix a handful of
real bugs found through hands-on testing, plus one new opt-in feature.
Grouped by area below.

### Pre-close warning
- **Now unconditional across both Security+ 1.0 and 2.0.** The original
  code only sounded a warning for Security+ 1.0 (by flashing the garage
  light, relying on the opener's own smart panel to beep in response) and
  silently trusted Security+ 2.0 openers to handle their own warning -
  which isn't true for every opener. The warning now always runs locally
  on the GDO blaQ's own onboard buzzer (GPIO4) and LED (GPIO3), regardless
  of protocol or what the opener itself does.

### Obstruction detection
- **Debounced against false positives.** A single stray reading no longer
  reaches HomeKit as "Obstructed" - two consecutive matching readings are
  required to confirm it. A Clear reading is still applied instantly, no
  debounce delay.
- **Now logged.** The obstruction event handler previously updated
  HomeKit silently with no log line at all, making it impossible to tell
  what triggered a given state from the logs. Every reading (confirmed,
  unconfirmed, or clear) now logs.
- **Staleness watchdog.** A confirmed "Obstructed" state now auto-clears
  after 30 seconds if no further reading arrives to back it up. gdolib
  doesn't send a steady heartbeat of obstruction status while idle, so a
  real detection could previously get confirmed correctly and then never
  receive a follow-up "Clear" - leaving the Home app stuck showing
  Obstructed indefinitely with no way to self-correct.

### Door state accuracy
- **Fixed a false "STOPPED" report.** If the discrete door-state field
  lagged behind the door's actual raw position (e.g. a status frame
  delayed by UART noise right as the door finished closing), the watchdog
  could previously mislabel a fully-closed door as "STOPPED" and trigger
  an unnecessary resync. It now falls back to raw position whenever the
  discrete field hasn't caught up yet, rather than only when it's
  reporting nothing at all.
- **Detects a refused/aborted close or open.** If the motor never engages
  at all (e.g. the opener's own safety circuit vetoes a close because the
  obstruction beam is already broken when the command arrives), HomeKit
  previously showed a permanent "Closing" spinner with nothing to correct
  it. This is now detected and HomeKit's target state is reverted to
  match reality.
- **Fast stall detection.** If the motor turns on but the door's position
  never actually moves, this is now detected in ~10 seconds instead of
  waiting out the full (much longer) normal travel-time timeout.

### GDO protocol detection (`gdolib`)
- **Fixed a false Security+ 1.0 lock-in.** Protocol detection previously
  trusted the byte-count of the very first UART read with no content
  validation - a single noise-induced 2-byte read could permanently lock
  the session into Security+ 1.0 even on a genuine Security+ 2.0 opener,
  for the rest of that boot. Detection now validates the actual byte
  content against the known command range before committing, so noise
  can no longer masquerade as a real protocol handshake.

### Auto-close (new, opt-in)
- Automatically closes the door after it's been continuously open for a
  configurable duration - **disabled by default.**
- When enabled, defaults to a 60-minute timeout.
- Skips closing (and re-checks every 5s rather than giving up) if the
  last known obstruction reading isn't Clear.
- Uses the exact same pre-close warning and close path as any other
  close, so all of the door-state watchdogs above apply to it
  automatically.
- Both the enabled/disabled toggle and the timeout duration are stored in
  NVS and exposed via `gdo_set_auto_close_enabled()` /
  `gdo_get_auto_close_enabled()` and `gdo_set_auto_close_timeout_ms()` /
  `gdo_get_auto_close_timeout_ms()`, ready to be wired up to a settings
  page on the diagnostics web server.

### Known issue: UART noise
Some installs may see frequent `RX data signature error` log lines,
especially correlated with the opener's motor running. This is most
likely EMI from the motor coupling onto the signal wire, not a firmware
bug - the protocol-detection fix above makes boot-time detection robust
against it, and the door-state watchdogs above make normal operation
tolerant of occasional dropped frames, but if you're seeing this
frequently it's worth checking wire routing (keep the signal line away
from motor/AC wiring), using a twisted pair, and confirming a solid short
ground connection to the opener.