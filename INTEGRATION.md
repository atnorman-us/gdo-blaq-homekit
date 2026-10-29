# Wiring pre_close_warning into gdo-blaq-homekit

I don't have network access in this sandbox, so I couldn't clone the actual
repo and give you exact line numbers — these two files (`pre_close_warning.h`
/ `pre_close_warning.c`) are a complete, self-contained module built from
the pin/timing values Konnected's own ESPHome firmware uses. Here's how to
wire it in.

## 1. Add the files

```
git clone --recurse-submodules https://github.com/konnected-io/gdo-blaq-homekit.git
cd gdo-blaq-homekit
cp /path/to/pre_close_warning.h main/
cp /path/to/pre_close_warning.c main/
```

Add `pre_close_warning.c` to `main/CMakeLists.txt`'s `SRCS` list (create one
if it doesn't already list source files individually — ESP-IDF component
CMakeLists usually glob `*.c`/`*.cpp`, in which case no edit is needed).

## 2. Verify the GPIO pins

The header hardcodes:
- `PRE_CLOSE_WARNING_BUZZER_GPIO` = 4 (matches `warning_beep_pin: GPIO4` in
  Konnected's `garage-door-GDOv2-Q.yaml`)
- `PRE_CLOSE_WARNING_LED_GPIO` = 2 (**placeholder** — the ESPHome warning-led
  package's pin wasn't visible to me; check `packages/warning-led.yaml` in
  `konnected-io/konnected-esphome` or the GDO blaQ schematic to confirm)

If `gdolib` already defines board pin constants (check for a `board.h` or
similar in the `gdolib` submodule/dependency), use those instead of the
hardcoded values so you don't drift from the canonical pinout.

## 3. Call `pre_close_warning_init()` once at boot

Find where other peripherals are initialized (likely `main/app_main.cpp`
or wherever `wifi_init()`-equivalent calls live, next to `main/wifi.cpp`'s
init call) and add:

```cpp
#include "pre_close_warning.h"
// ...
pre_close_warning_init();
```

## 4. Find the door-close write path and call the warning first

You're looking for the HomeKit characteristic write callback for the
Garage Door Opener service's **Target Door State** characteristic — this
is what fires when someone toggles the door in the Home app. In
esp-homekit-sdk-based accessories this is typically a callback registered
against `HOMEKIT_CHARACTERISTIC_TARGET_DOOR_STATE`, often named something
like `door_target_state_write` or found in a file like
`main/accessory.cpp` / `main/door.cpp` / `main/garage_door.cpp`.

Inside that callback, before calling into `gdolib` to actually send the
close command, add:

```cpp
if (requested_state == HOMEKIT_CHARACTERISTIC_TARGET_DOOR_STATE_CLOSED &&
    current_state == HOMEKIT_CHARACTERISTIC_CURRENT_DOOR_STATE_OPEN) {

    // Blocking version — simplest, but only safe if this callback
    // doesn't need to return quickly:
    pre_close_warning_run(PRE_CLOSE_WARNING_DURATION_MS);

    // OR non-blocking version if HAP write callbacks in your
    // esp-homekit-sdk version need to return promptly:
    // pre_close_warning_run_async(PRE_CLOSE_WARNING_DURATION_MS, issue_close_command);
    // (and move the actual gdolib close call into issue_close_command)
}

// existing code that calls gdolib's close function goes here (unchanged)
```

If you paste me the actual contents of that callback (or the whole
`main/` directory listing) I can turn this into an exact diff instead of
a generic patch.

## 5. Also fire it from a standalone "warn" trigger (optional)

If you want a `/button/Pre-close Warning/press`-equivalent HomeKit-exposed
stateless trigger — same as the ESPHome API endpoint — add a HomeKit
`Switch` or `Stateless Programmable Switch` service/accessory whose write
handler calls `pre_close_warning_run_async(PRE_CLOSE_WARNING_DURATION_MS, NULL)`
without touching the door state at all. That mirrors the "trigger the
warning independently of a close command" behavior documented in
Konnected's REST API.

## 6. Build and flash

```
idf.py build
idf.py -p /dev/tty.usbXXXX flash monitor
```

Watch the monitor log for `pre-close warning: running for 5000 ms` when
you toggle the door closed from the Home app, and confirm you hear/see
the buzzer and LED before the door actually starts moving.
