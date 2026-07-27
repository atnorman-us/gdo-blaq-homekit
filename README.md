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
Open in a browser, on the same network as the device: **`http://gdo-blaq.local:8080/`**

If mDNS/Bonjour resolution isn't available on your network, use the device's IP instead — same one your router/Home app already knows it by (or check the boot log for `esp_netif_handlers: sta ip: ...`):

URLDashboard(
status + live log, auto-refreshing) --> `http://<device-ip>:8080/`

Raw status as JSON --> `http://<device-ip>:8080/status`

Log buffer, plain text (view/copy) --> `http://<device-ip>:8080/logs`

Log buffer, forced download --> `http://<device-ip>:8080/logs/download` → saves gdo-log.txt

## On the dashboard (/):
Status table: door / light / lock / obstruction / motion state, uptime, free heap, WiFi RSSI, connected HomeKit sessions, last reset reason, current log buffer usage, running/other OTA partition and firmware version.

Auto-Close: enable/disable and set the open-duration timeout, with a live countdown once armed.

Firmware: upload a new `.bin` and roll back to the previously-running image — see [Updating firmware over the air (OTA)](#updating-firmware-over-the-air-ota) below.

Log pane: shows the last ~64KB of log history, refreshes every 3s by default (toggle off with the checkbox if you want to read a specific moment without it jumping).

Download full log button — same as hitting /logs/download directly, gives you a .txt file

## Updating firmware over the air (OTA)

Once a device is running 1.2 or later, new firmware can be uploaded straight from the diagnostics dashboard — no USB cable needed.

1. Build the new firmware: `idf.py build` (output at `build/gdo-blaq-homekit.bin`).
2. Open the dashboard's **Firmware** section, choose the `.bin`, and click **Upload**.
3. The device writes the image to its inactive OTA slot and validates it, then reports the detected version *before* anything is committed.
4. Click **Confirm & Reboot** to boot into it, or **Cancel** to back out — either way the currently-running firmware is untouched until you confirm.
5. If a newly-booted image can't bring WiFi and the diagnostics server back up, the bootloader automatically reverts to the previous image on the next reset — no scenario should leave the device permanently stuck on a bad update.
6. **Roll back to other slot** lets you deliberately switch back to the previously-running image at any time, without needing a fresh upload.

**One-time requirement for devices still on 0.5 or earlier:** OTA needs a two-slot partition table that older builds didn't have. A device on an older build needs **one manual USB flash** (`idf.py flash`) to move onto the new layout — WiFi credentials and saved settings are preserved across this transition. Every update after that one-time step can go entirely through the web page.

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
- **Detects a refused/aborted close or open, and retries once.** If the
  motor never engages at all (e.g. the opener's own safety circuit vetoes
  a close because the obstruction beam is already broken when the command
  arrives), the command is now automatically re-sent once before giving
  up. HomeKit's target state is only reverted to match reality if the
  retry also goes unanswered - previously it reverted immediately with no
  attempt to just try again, and before that fix existed at all it showed
  a permanent "Closing" spinner with nothing to correct it.
- **Fast stall detection.** If the motor turns on but the door's position
  never actually moves, this is now detected in ~10 seconds instead of
  waiting out the full (much longer) normal travel-time timeout.

### GDO protocol detection
- **Fixed a false Security+ 1.0 lock-in (`gdolib`).** Protocol detection
  previously trusted the byte-count of the very first UART read with no
  content validation - a single noise-induced 2-byte read could
  permanently lock the session into Security+ 1.0 even on a genuine
  Security+ 2.0 opener, for the rest of that boot. Detection now validates
  the actual byte content against the known command range before
  committing, so noise can no longer masquerade as a real protocol
  handshake.
- **Protocol is now pinned from the last confirmed sync, not
  re-auto-detected every boot.** Even with the fix above, Security+ 1.0
  detection is inherently a weaker signal than Security+ 2.0's - a single
  plausible-looking packet within a few seconds, versus 2.0's full
  multi-stage handshake. Once a device has synced successfully as
  Security+ 2.0, that result is saved and pinned on every future boot,
  skipping auto-detection entirely. A Security+ 1.0 sync only gets saved
  if nothing (or already Security+ 1.0) is on record, so it can never
  overwrite a proven Security+ 2.0 pairing on the same hardware - real
  installations don't change protocol in the field. Security+ 1.0
  hardware that's never synced as 2.0 is unaffected and still goes
  through gdolib's normal auto-detection.

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
  NVS via `gdo_set_auto_close_enabled()` / `gdo_get_auto_close_enabled()`
  and `gdo_set_auto_close_timeout_ms()` / `gdo_get_auto_close_timeout_ms()`,
  and configurable live from the diagnostics dashboard's Auto-Close
  section.

### Firmware updates (new)
- **Over-the-air updates from the diagnostics dashboard** - upload a new
  `.bin` from a browser instead of needing a USB cable for routine
  updates. See [Updating firmware over the air (OTA)](#updating-firmware-over-the-air-ota)
  above for the full flow, including the two-step confirm/cancel upload,
  manual rollback, and the automatic boot-rollback safety net for a bad
  image.

### mDNS (new)
- The device now advertises itself as `gdo-blaq.local` on the LAN, so the
  diagnostics dashboard no longer requires hunting down its IP address.

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