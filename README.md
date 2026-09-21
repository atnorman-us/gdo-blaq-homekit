# HomeKit Firmware for the GDO blaQ

Native HomeKit firmware for Konnected's GDO blaQ, supporting Chamberlain, LiftMaster, Craftsman, and Merlin garage door openers using Security+ or Security+ 2.0.

**Current release: [1.3.1.10](https://github.com/atnorman-us/gdo-blaq-homekit/releases/tag/1.3.1.10)**

## What's changed in 1.3.1.10

- Reconcile obstruction state from every validated status frame, not only when the baseline is unknown or right after a toggle event. An obstruction toggle event can go stale if its matching event frame is lost to RX noise; the following status frame's obstruction bit now corrects it instead of leaving the stale reading in place.

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
2. Open **`http://192.168.4.1`**, enter your WiFi credentials, and select **Write and Reboot**.
3. Once the device joins your network and finishes starting, open Apple's Home app, select **Add Accessory**, then **More Options**.
4. Select the accessory and enter setup code **`251-02-023`**.

This is native HomeKit firmware. Release 1.3.1.9 does not introduce a Matter migration or claim device-tested iOS 27 certification.

## Diagnostics dashboard and admin password

Open **`http://gdo-blaq.local:8080/`** on the same network as the device. If mDNS does not resolve, use **`http://<device-ip>:8080/`** instead.

On first use, set an **8–64-character admin password** using the dashboard's setup banner. The dashboard prompts with a masked password field for protected actions and retains the entered password only in the current page's memory. Saved credentials survive a reboot. This replaces the earlier generated admin-token workflow; an older token is not the new admin password.

The password protects settings, restart, firmware upload/confirmation/cancellation/rollback, and log viewing/download. The dashboard and `/status` remain readable without it. These endpoints use local HTTP, so use them on a trusted LAN and do not expose port 8080 to the internet.

There is no dashboard password-reset feature. Erasing NVS to recover a forgotten password also removes WiFi, HomeKit, and opener-related saved state and requires reconfiguration.

| Endpoint | Purpose | Password required |
| --- | --- | --- |
| `/` | Dashboard | For protected actions |
| `/status` | Current status as JSON | No |
| `/logs` | Log buffer as plain text | Yes |
| `/logs/download` | Download `gdo-log.txt` | Yes |

Use the dashboard's log controls to supply authentication automatically. API clients must send the admin password in the `X-GDO-Token` request header; opening a protected log URL directly does not supply that header.

The dashboard includes door/light/lock state, motion and obstruction, battery status, lifetime openings, WiFi signal and IP address, memory, HomeKit sessions, reset reason, RX errors, auto-close settings, firmware controls, and a log buffer of approximately 64 KB. A pairing-fault banner appears after prolonged sync failure.

## Updating firmware over the air (OTA)

Download files from the [GitHub releases page](https://github.com/atnorman-us/gdo-blaq-homekit/releases).

| Release file | Use |
| --- | --- |
| `gdo-blaq-homekit-1.3.1.9-ota.bin` | App-only image for the dashboard's firmware uploader |
| `konnected-gdo-blaq-homekit-1.3.1.9.bin` | Combined image for USB installation at flash address `0x0` |
| `SHA256SUMS.txt` | Checksums for both images |

**Upload the OTA image to the dashboard, not the combined image.** When building locally, the OTA image is `build/gdo-blaq-homekit.bin`.

1. Open the dashboard's **Firmware** section and select the OTA `.bin`.
2. Select **Upload** and enter the admin password when requested.
3. Review the detected version after the image has been written and validated in the inactive OTA slot.
4. Select **Confirm & Reboot**, or **Cancel** to keep running the current image.
5. After reboot, check the reported version and test door state and controls.

Web OTA accepts ordinary ESP-IDF application images; this release does not require cryptographically signed firmware. Image validation is not publisher authentication, so obtain firmware from this repository's releases.

A new OTA image is marked valid only after WiFi and the diagnostics server start. If it resets before being marked valid, bootloader rollback can restore a valid previous image. This is not a guarantee of recovery from every failure. **Roll back to other slot** is also available when the other slot contains a usable image.

Older firmware without a compatible two-slot OTA partition layout needs a USB installation first. Back up any needed information and expect to reconfigure if changing layouts; a combined-image flash also replaces OTA metadata.

## USB installation

A browser-based installer is also available at [espweb.terazone.com](https://espweb.terazone.com/) as an alternative to the manual `esptool`/USB steps below; see [`espweb/README.md`](espweb/README.md) for details on that site.

For a source build, connect the device by USB and run:

```sh
idf.py -p <serial-port> flash
```

For a downloaded combined image, use ESP-IDF's esptool environment:

```sh
python -m esptool --chip esp32s3 --port <serial-port> write_flash 0x0 konnected-gdo-blaq-homekit-1.3.1.9.bin
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

- **UART errors:** Rejected or malformed frames are ignored and logged. Repeated errors can indicate wiring or electrical noise; inspect signal routing, connections, and grounding.
- **Pairing fault:** Check opener power, wiring, and whether it still recognizes the controller. The banner is based on sustained synchronization failure, not a definitive diagnosis of lost pairing.
- **Home app state:** State tracking and watchdog recovery have been improved, but hardware and Home hub behavior still need testing on your installation. Repeatedly submitting an unchanged value does not force the HomeKit SDK to send another notification.
- **After an update:** Verify local and remote Home control, final door state, obstruction reporting, and cancellation during the warning before relying on unattended automatic closing.
