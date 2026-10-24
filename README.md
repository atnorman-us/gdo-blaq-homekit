# HomeKit Firmware for the GDO blaQ

Native HomeKit firmware for Konnected's GDO blaQ, supporting Chamberlain, LiftMaster, Craftsman, and Merlin garage door openers using Security+ or Security+ 2.0.

**Current release: [1.3.7.1](https://github.com/atnorman-us/gdo-blaq-homekit/releases/tag/1.3.7.1)**

## What's changed in 1.3.7.1

Rebuilt in place with direct OTA downloads from espweb.terazone.com, avoiding GitHub's signed redirects, and an editable **Firmware URL** field. Downloads require verified HTTPS and a binary response; activation still requires a separate confirmation. The reported HTTPS download crash still needs hardware verification.

Orderly software restarts now disconnect the opener UART transmit signal and release TX/RX as inputs before the UART peripheral resets. This covers firmware confirmation, manual restart, and software rollback through `esp_restart()`. Registration is required before initializing the opener interface. This mitigation is not yet verified on hardware and does not cover power loss, hard resets, or connecting control wires. The reported Security+ 1.0 opening-on-reboot issue remains unconfirmed as resolved. **The first update into this version still reboots through the old firmware, which lacks this protection.**

## What's changed in 1.3.7.0

### Update from GitHub release

The Firmware card now offers **Update from GitHub release**. It looks up the latest stable release, shows release notes, and downloads the OTA image directly to the device over certificate-verified HTTPS. Existing image validation and the separate Confirm/Cancel step remain: downloading does not activate the image or reboot. Manual file upload remains available if lookup or download fails. Internet access and a correct device clock are required.

### Known limitation

The reported Security+ 1.0 opening-on-reboot issue remains unresolved. Avoid updating or rebooting affected Security+ 1.0 installations pending a fix. Direct GitHub downloading has not yet been validated on physical hardware.

## What's changed in 1.3.6.1

### Security+ 2.0 obstruction reporting

- Request fresh opener status after OBST_1 and OBST_2 events instead of leaving HomeKit obstructed until the next periodic report.
- Combine duplicate events into one confirmation cycle, with at most three requests spaced one second apart.
- Once obstruction is known, events can assert obstruction but cannot clear it by toggling. Only a valid status response reporting Clear clears the flag; missing responses leave it unchanged.

### Known limitation

The reported Security+ 1.0 door opening during firmware-update reboot remains unresolved. This release does not fix that issue. Avoid updating or rebooting affected Security+ 1.0 installations pending a fix.

## What's changed in 1.3.6.0

- Schedule edits and scheduled-close decisions are synchronized, including timezone changes. A removed schedule cannot arm under a newer settings revision.
- HomeKit target changes share one ordered queue, preventing an older queued update from overwriting a newer command.
- Settings and password writes report storage errors instead of claiming success. Schedule and credential records are saved together; existing settings are read on upgrade.
- **Recent activity** shows the latest 32 events: HomeKit requests, observed door/obstruction changes, warning cancellations, retries, and scheduled-close skip reasons. It is password-protected, held in RAM, and resets at reboot. Consecutive identical events are collapsed. Times use the device clock when available, otherwise uptime. Observed opener events do not identify whether MyQ, a remote, or a wall control caused them.
- **Change admin password** requires the existing password and confirmation of the new password. It preserves Wi-Fi, pairing, and other settings. Other browser pages must authenticate again with the new password.
- **Firmware review survives a page refresh:** pending version, Confirm/Cancel, time remaining, and rollback availability are restored from the device. A cancelled, failed, or expired upload remains ineligible for rollback across reboot. Reboot clears pending confirmation; upload again to review it.
- **Rollback compatibility:** only an image that has successfully booted with the new settings format is offered for manual rollback. An older or unknown slot is unavailable because booting it could restore obsolete passwords or schedules. Automatic bootloader recovery remains separate.

## What's changed in 1.3.5.0

Opener link:

- **Spurious Locked/Unlocked (and Light) flaps on Security+ 1.0 openers with a smart wall panel are gone.** In the listen-only "with smart panel" mode an idle door produces no state-change events, so after five quiet minutes the watchdog sent a status request of its own. That request was a Security+ 2.0 command id the 1.0 bus does not know, it collided with the panel's polls, and one misread status byte showed up in the Home app as a lock that locked and unlocked itself half a second apart, every five minutes. The watchdog now treats any decoded packet as proof of life (the panel's polls count), a status request is never sent in listen-only mode and is a proper 1.0 query otherwise, and light and lock changes on Security+ 1.0 are reported only once two consecutive polls agree.

## What's changed in 1.3.4.0

Wi-Fi setup (the `konnected-blaq-hk` access point and its page at `http://192.168.4.1`):

- **HomeKit no longer starts while the device is in setup (access point) mode.** Its HTTP server and the setup page both want port 80; whichever bound first won, so the setup page at `http://192.168.4.1` was sometimes unreachable, and otherwise HomeKit logged `HTTPD START Failed` on every restart attempt. HomeKit starts normally once the device has joined a network and rebooted.
- The setup server no longer shuts down when a device leaves the access point. It was stopped on every station disconnect, which dropped the page's WebSocket for anyone else still using it (for example, a laptop mid-setup when a phone wandered off).
- **A device that failed to join your Wi-Fi no longer starts an access point named after your own network.** A precedence bug in the provisioning library made it clone your SSID *and* password as its own open-to-join AP; other devices (including a second blaQ) could roam onto it and lose the internet. It now falls back to the default `konnected-blaq-hk` AP, as documented.
- The HTTP server's request-header limit is raised from 512 to 1024 bytes. Chrome and Safari send about 490 bytes of headers on a WebSocket upgrade, so the setup page's connection was one header away from a `431` rejection; `curl` and small scripts were unaffected.
- The setup server now recycles idle browser connections (`lru_purge_enable`), so a few page reloads can no longer exhaust its seven sockets and make the device refuse new connections.
- The setup page no longer reloads itself forever when its WebSocket drops; it reconnects a few times and then tells you what to check. It also shows a confirmation after **Save and reboot**, masks password fields, and keeps a saved password when the field is left blank.
- Passwords and SSIDs containing `"` or `\` are stored correctly (JSON escapes were previously written to flash verbatim), and SSIDs are escaped on the way out.
- The stored Wi-Fi password is no longer sent to whoever opens the setup page. SSIDs of 32 characters no longer overflow a 32-byte buffer.
- A station configured with an empty password now connects to an open network instead of being rejected by the WPA2 threshold.
- The network watchdog no longer prods the station interface every 5 seconds while the device is in setup (AP) mode, and no longer treats 0.0.0.0 as a valid address.

## What's changed in 1.3.3.0

Opener link:

- **A real Security+ 1.0 wall panel (for example an 889LM) is now detected even when it starts talking after the blaQ does.** The startup listen window grows from 2.5 to 8 seconds, and if the blaQ is already emulating a panel it counts the polls it sees against the polls it sent; a surplus means a real panel is on the wire, so it stops emulating and switches to the listen-only "Security+ 1.0 with smart panel" mode, which is then saved for the next boot. Two panels polling the same wire made the real one beep and flash.
- **Protocol detection re-runs on its own if the Security+ 2.0 fallback never syncs.** "Security+ 2.0" is only what the driver assumes when the 1.0 probes hear nothing, for instance because the opener was wired up after boot; it used to stay there until the next reboot.
- **A door command is no longer retried on Security+ 1.0 openers.** Those openers only have a TOGGLE command, chosen from the door state the firmware last saw. Re-sending it 8 seconds after a command that appeared not to start could stop or reverse a door that had in fact started moving (or whose status frame was missed), including closing an open door with no warning. On these openers an unanswered command now just reverts the Home app's target; Security+ 2.0 keeps its single retry, since its Open/Close commands are idempotent.
- gdolib now decodes every packet in a UART read that holds more than one frame (two breaks followed by one 38-plus-byte read). Previously only the one-break case was handled this way and the older fallback discarded the whole first packet.
- gdolib no longer leaks a packet buffer when its transmit queue is full.

Door control and HomeKit:

- A lock change made at the wall panel now updates Lock Target State too, so the Home app no longer shows "Locking..." or "Unlocking..." indefinitely.
- The accessory name now uses device-unique MAC bytes instead of the Espressif vendor prefix, so two blaQs no longer share a name. Light and Lock writes that the opener refuses are reported to the Home app instead of silently ignored.
- A close that is superseded during its warning (by an obstruction or a settings change) puts the Home app's target back instead of leaving it on "Closing...". A close for a door that is already closed skips the warning.
- HomeKit notifications are deduplicated, the queue is larger, and dropped notifications are retried, so a single door transition can no longer starve later state updates.
- Changing the scheduled-close time on a day it already fired re-arms it, and an auto-close that the opener accepted but never acted on no longer blocks auto-close and scheduled close until the door moves.

Diagnostics page:

- New **Beep before closing** switch in the Auto-Close card mutes the pre-close beep; the LED flash and cancel window stay. It is on by default.
- The admin password must be printable ASCII without leading or trailing spaces, so a password can no longer be set that the browser cannot send back. 0 auto-close minutes is rejected, and the scheduled-close timezone must be a POSIX string with a UTC offset; names such as `EST` or `America/New_York` were previously accepted but silently read as UTC.
- Security: `/admin/set-password`, the one unauthenticated write, now refuses requests from other sites and from non-local Host names, and rejects oversized `Host`, `Origin` and `Sec-Fetch-Site` headers, so a web page elsewhere on the LAN cannot claim a fresh device.
- "Roll back to other slot" refuses to boot an upload that was cancelled or never confirmed, and a failed server restart after a Wi-Fi bounce is retried.

## What's changed in 1.3.2.0

- Add **Scheduled Close**: close the garage door at a set time of day (for example 23:00) if it is open, using the same beep-and-flash warning as Auto-Close. It is off by default and configured on the diagnostics page, where the timezone can be filled in from your browser. The device sets its clock over Wi-Fi (`pool.ntp.org`) and does nothing until the clock is synced. A door that was closed at the set time, or opened after it, is left alone. See [Scheduled close](#scheduled-close).

## Build from source

Use **ESP-IDF 5.4.1** with the **ESP32-S3** target.

```sh
git clone --recurse-submodules https://github.com/atnorman-us/gdo-blaq-homekit.git
cd gdo-blaq-homekit
# Activate your ESP-IDF environment before running these commands.
idf.py set-target esp32s3
idf.py build
```

For an existing clone, initialize dependencies with:

```sh
git submodule update --init --recursive
```

CMake applies the fixes in `patches/` automatically. The two patched dependency submodules may therefore appear modified after configuration; their changes are maintained in the parent repository's patch files.

The release version comes from `version.txt`. To run the host regression tests, use Python 3, a C/C++ compiler with AddressSanitizer/UndefinedBehaviorSanitizer support, and Node.js for the dashboard tests:

```sh
python3 -m unittest discover -s tests -v
```

## Provision WiFi and add to HomeKit

1. Connect to the device's **`konnected-blaq-hk`** WiFi access point.
2. Open **`http://192.168.4.1`**, enter your WiFi credentials, and select **Save and reboot**. If a browser shows an error but the address bar has changed to `https://`, the browser is replaying a cached redirect from another device that uses the same address (UniFi gateways, for example); open the page in a private window or clear that site's data.
3. Once the device joins your network and finishes starting, open Apple's Home app, select **Add Accessory**, then **More Options**.
4. Select the accessory and enter setup code **`251-02-023`**.

This is native HomeKit firmware. Release 1.3.5.0 does not introduce a Matter migration or claim device-tested iOS 27 certification.

## Diagnostics dashboard and admin password

Open **`http://gdo-blaq.local:8080/`** on the same network as the device. If mDNS does not resolve, use **`http://<device-ip>:8080/`** instead.

On first use, set an **8–64-character admin password** using the dashboard's setup banner. The dashboard prompts with a masked password field for protected actions and retains the entered password only in the current page's memory. Saved credentials survive a reboot. This replaces the earlier generated admin-token workflow; an older token is not the new admin password.

The password protects settings, restart, firmware upload/confirmation/cancellation/rollback, and log viewing/download. The dashboard and `/status` remain readable without it. These endpoints use local HTTP, so use them on a trusted LAN and do not expose port 8080 to the internet.

Use **Change admin password** to rotate a known password without erasing settings. There is no forgotten-password reset in the dashboard. Erasing NVS to recover a forgotten password also removes WiFi, HomeKit, and opener-related saved state and requires reconfiguration.

| Endpoint | Purpose | Password required |
| --- | --- | --- |
| `/` | Dashboard | For protected actions |
| `/status` | Current status as JSON | No |
| `/logs` | Log buffer as plain text | Yes |
| `/logs/download` | Download `gdo-log.txt` | Yes |
| `/activity` | Recent structured activity as JSON | Yes |

Use the dashboard's log controls to supply authentication automatically. API clients must send the admin password in the `X-GDO-Token` request header; opening a protected log URL directly does not supply that header.

The dashboard includes door/light/lock state, motion and obstruction, battery status, lifetime openings, WiFi signal and IP address, memory, HomeKit sessions, reset reason, RX errors, auto-close settings, firmware controls, and a log buffer of approximately 64 KB. A pairing-fault banner appears after prolonged sync failure.

## Scheduled close

Off by default. On the diagnostics page, **Scheduled Close** closes the garage door at a set time of day (for example 23:00 every night) if the door is open at that time. It uses the same beep-and-flash warning as Auto-Close, which plays before the door closes.

- **Time sync:** the device gets the time over Wi-Fi from `pool.ntp.org`. Until it has synced, the schedule does nothing and the card shows "Clock not synced yet".
- **Timezone:** choose **Use browser timezone**, or enter a POSIX timezone string such as `PST8PDT,M3.2.0,M11.1.0`. **Use browser timezone** only recognizes about 20 common zones, so use a POSIX string for any other zone.
- **Timezone default:** until you save a timezone, the device uses UTC, so a saved time is read as UTC. The page pre-fills your browser's timezone when it recognizes it, but nothing takes effect until you save. After saving, check **Device time now** on the card to confirm it matches your clock.
- **Timing:** the close runs at most once per night. The door must already be open when the set time arrives, but the close can be picked up during the following five minutes (for example, after a brief Wi-Fi drop). A door that was closed at the set time, or opened after it, is left alone until the next night. A device reboot during that window skips that night's close. A door stopped partway (not fully open) is not closed.
- **Saving inside the window:** if you save a schedule while the door is already open and you are inside an open five-minute window, the door closes within seconds.
- **Daylight saving:** times from 02:00 to 02:59 do not fire on the night daylight saving time springs forward, because that local time does not exist.
- **Obstruction:** if the obstruction sensor is not clear, the close is skipped and retried for the rest of the five-minute window.

## Updating firmware over the air (OTA)

Download files from the [GitHub releases page](https://github.com/atnorman-us/gdo-blaq-homekit/releases).

| Release file | Use |
| --- | --- |
| `gdo-blaq-homekit-1.3.5.0-ota.bin` | App-only image for the dashboard's firmware uploader |
| `konnected-gdo-blaq-homekit-1.3.5.0.bin` | Combined image for USB installation at flash address `0x0` |
| `SHA256SUMS.txt` | Checksums for both images |

**Upload the OTA image to the dashboard, not the combined image.** When building locally, the OTA image is `build/gdo-blaq-homekit.bin`.

1. Open the dashboard's **Firmware** section and select the OTA `.bin`.
2. Select **Upload** and enter the admin password when requested.
3. Review the detected version after the image has been written and validated in the inactive OTA slot.
4. Select **Confirm & Reboot**, or **Cancel** to keep running the current image.
5. After reboot, check the reported version and test door state and controls.

Web OTA accepts ordinary ESP-IDF application images; this release does not require cryptographically signed firmware. Image validation is not publisher authentication, so obtain firmware from this repository's releases.

A new OTA image is marked valid only after WiFi and the diagnostics server start. If it resets before being marked valid, bootloader rollback can restore a valid previous image. This is not a guarantee of recovery from every failure. **Roll back to other slot** is also available when the other slot contains a usable image.

Older firmware without a compatible two-slot OTA partition layout needs a USB installation first. **A combined-image USB flash erases the saved settings**: the image covers the NVS partition at `0x9000`, so Wi-Fi credentials, the admin password, and the HomeKit pairing are gone and the device boots into the `konnected-blaq-hk` setup access point. Only the dashboard's OTA upload keeps them. A combined-image flash also replaces OTA metadata.

## USB installation

A browser-based installer is also available at [espweb.terazone.com](https://espweb.terazone.com/) as an alternative to the manual `esptool`/USB steps below; see [`espweb/README.md`](espweb/README.md) for details on that site.

For a source build, connect the device by USB and run:

```sh
idf.py -p <serial-port> flash
```

For a downloaded combined image, use ESP-IDF's esptool environment:

```sh
python -m esptool --chip esp32s3 --port <serial-port> write_flash 0x0 konnected-gdo-blaq-homekit-1.3.5.0.bin
```

To generate a combined image locally with the correct partition offsets:

```sh
python scripts/merge-firmware.py build build/konnected-gdo-blaq-homekit.bin
```

## Pre-close warning and automatic closing

The GDO blaQ's onboard buzzer and LED provide a five-second warning before an app-requested close, for both supported protocols. The HomeKit request thread remains available during that warning so a subsequent Open command can cancel the pending close.

Automatic closing is **disabled by default**. Enable it and choose a timeout in the dashboard; its default timeout is 60 minutes. The door must remain continuously open, and the controller checks obstruction, synchronization, settings, and newer commands before issuing the close after its warning. Settings persist across reboots.

A real obstruction is reported immediately and is not automatically declared clear after a fixed interval. At boot, an unknown obstruction baseline is resolved from status rather than inferred from an ambiguous toggle.

## Troubleshooting

- **Security+ 1.0 opener with a smart wall panel (880LM, 889LM, 41A7305 and similar):** power the blaQ from its USB-C port, not from the opener's wall-control terminals. Those openers supply very little power on that pair, and a blaQ drawing from it can keep the panel from recharging; an 889LM then blinks red and yellow and beeps indefinitely, and never starts talking, so the blaQ falls back to emulating a panel instead of listening to the real one. The dashboard's protocol field shows **Security+ 1.0 with smart panel** once the panel is heard.

- **UART errors:** Rejected or malformed frames are ignored and logged. Repeated errors can indicate wiring or electrical noise; inspect signal routing, connections, and grounding.
- **Pairing fault:** Check opener power, wiring, and whether it still recognizes the controller. The banner is based on sustained synchronization failure, not a definitive diagnosis of lost pairing.
- **Home app state:** State tracking and watchdog recovery have been improved, but hardware and Home hub behavior still need testing on your installation. Repeatedly submitting an unchanged value does not force the HomeKit SDK to send another notification.
- **After an update:** Verify local and remote Home control, final door state, obstruction reporting, and cancellation during the warning before relying on unattended automatic closing.

### Update from GitHub release

The Firmware card can look up the latest stable GitHub release and download its OTA image directly to the device over verified HTTPS. Review the release notes before downloading. The existing Confirm/Cancel step still controls reboot; downloading alone does not activate the image. Internet access and a correct device clock are required. File upload remains available as a fallback.

### Direct OTA mirror

The latest-release updater still reads GitHub release metadata but downloads the selected OTA image directly from `https://espweb.terazone.com/firmware/`. It rejects redirects and non-binary responses, retains HTTPS certificate validation, and uses the existing Confirm/Cancel flow. The release workflow uploads both OTA and USB images to R2 before advancing the manifest. Direct download removes the signed GitHub redirect; the previously reported HTTPS crash still needs hardware verification.

### Custom firmware URL

The Firmware card includes an editable Firmware URL field and Download from URL button, alongside latest-release and file-upload options. Supply a direct HTTPS OTA `.bin` URL (up to 1024 characters); redirects and non-binary responses are rejected. Downloading requires the admin password and does not reboot. Review the validated version and explicitly Confirm or Cancel. The download shows stage text, not a byte-progress percentage.
