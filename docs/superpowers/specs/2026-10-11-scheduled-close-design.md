# Scheduled Close — Design

## Goal

If the garage door is open at a user-set time of day (e.g. 23:00), the device
closes it automatically, using the same warning-then-close sequence as the
existing auto-close.

## Decisions

- Local time comes from SNTP plus a user-set POSIX timezone string (handles DST).
- One daily time, one enable switch. No per-day or multiple schedules (YAGNI).
- Trigger: a scheduler that polls the clock about every 30 s.
- Out of scope: HomeKit exposure, multiple/per-day schedules, notifications.

## Components

### Time sync
- Start SNTP (lwIP already built with it) after Wi-Fi is up, server `pool.ntp.org`.
- Apply timezone with `setenv("TZ", ...)` + `tzset()` at boot and whenever it changes.
- Clock is "synced" only if the year is >= 2024. Until then the scheduler does nothing.

### Settings (NVS, existing settings namespace)
| Key | Type | Meaning | Default |
|---|---|---|---|
| `sched_en` | u8 | schedule enabled | 0 |
| `sched_min` | u16 | minutes after local midnight, 0–1439 | 1380 (23:00) |
| `tz` | string | POSIX TZ string | `UTC0` |
| `sched_last_day` | i32 | local day number of last fire | 0 |

Accessors are declared in `main/gdo_settings.h` and defined in
`gdo-blaq-homekit.cpp`, following the auto-close accessors' pattern
(update in-memory value, then persist; validate input).

### Scheduler (`main/close_schedule.c/.h`)
Pure function, host-testable:

`should_fire(now_minute_of_day, today_day_number, sched_minute, last_fired_day, door_is_open, clock_synced)`

Returns true when: clock synced, door open, `now` is within a window of
`[sched_minute, sched_minute + 5)` (wrapping at midnight), and
`last_fired_day != today`. The window means a reboot or brief Wi-Fi drop
around the scheduled time does not skip the close, but it never fires hours late.

A task polls every ~30 s; on true it records `sched_last_day = today`,
then runs the existing sequence: pre-close warning, then `gdo_control_close()`.
If the door is closed or moving in the window, nothing happens until the next day.
`sched_last_day` is persisted only when it fires, to limit flash wear.

### Web UI / API
- Diagnostics page: new "Scheduled close" block beside Auto-close with an enable
  toggle, time picker, and timezone field. Timezone is pre-filled from the browser
  using a small built-in map of common IANA zones to POSIX strings; the field
  remains editable.
- `POST /settings/close-schedule` (`enabled`, `time` HH:MM, `tz`), protected the
  same way as the other settings POSTs; validates time range and TZ string
  length/charset.
- `/status` JSON gains: schedule enabled, scheduled time, tz, clock-synced flag,
  device local time.

## Error handling
- Invalid time or TZ: 400, nothing changed.
- NVS write failure: report error, in-memory value reverted (as auto-close does).
- SNTP unreachable: scheduler idle; `/status` shows clock not synced.

## Testing
- Host-side Python tests (like existing `tests/`) for `should_fire`: window edges,
  midnight wrap, already-fired, door open/closed, unsynced clock.
- Test that new settings keys and values are validated and don't leak into logs.
