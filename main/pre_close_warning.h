// pre_close_warning.h
//
// Drop-in pre-close warning module for konnected-io/gdo-blaq-homekit.
// Ported concept from the ESPHome secplus_gdo component's
// `garage_door_close_warning_duration` behavior (see
// konnected-io/konnected-esphome: packages/warning-led.yaml,
// packages/buzzer-rtttl.yaml).
//
// Usage: call pre_close_warning_run() BEFORE issuing the close command
// to gdolib, and only when the door is currently open. It blocks for
// the warning duration, pulsing the buzzer and strobing the LED, then
// returns so the caller can proceed with the actual close.
//
// This satisfies the same intent as 16 CFR 1211.14(f) (audible + visual
// warning prior to unattended/remote close) that the ESPHome firmware
// already implements.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// GPIO assignments confirmed from Konnected's GDOv2-Q ESPHome hardware
// mapping (garage-door-GDOv2-Q.yaml, "INTERNAL MAPPINGS - DO NOT EDIT"
// block): uart_tx_pin: GPIO1, uart_rx_pin: GPIO2, warning_beep_pin: GPIO4,
// warning_leds_pin: GPIO3, status_led: GPIO18, input1: GPIO5, input2: GPIO9.
//
// Note: gdolib itself only owns GPIO1/GPIO2 (UART TX/RX to the opener) —
// it has no concept of the buzzer or LED. Those are driven independently
// by application code (this module), which is why GPIO3/GPIO4 are safe to
// claim here without touching gdolib's pins.
#define PRE_CLOSE_WARNING_BUZZER_GPIO   4
#define PRE_CLOSE_WARNING_LED_GPIO      3

// Default warning duration, matches ESPHome default (garage_door_close_warning_duration: 5s)
#define PRE_CLOSE_WARNING_DURATION_MS   5000
#define PRE_CLOSE_WARNING_BEEP_ON_MS    250
#define PRE_CLOSE_WARNING_BEEP_OFF_MS   250

// Call once at startup (e.g. from app_main / accessory init) to configure
// the buzzer (LEDC) and LED (GPIO) peripherals.
void pre_close_warning_init(void);

// Blocking call: runs the beep/flash sequence for duration_ms.
// Call this from the HomeKit "Target Door State" write callback,
// BEFORE calling the gdolib close function, whenever the requested
// state is CLOSED and the current state is OPEN.
void pre_close_warning_run(uint32_t duration_ms);

// Non-blocking variant: starts the warning on a FreeRTOS timer/task and
// invokes on_complete() when done. Use this if blocking inside the
// HomeKit characteristic write callback causes HAP timeouts on your
// esp-homekit-sdk version (some versions expect writes to return quickly).
void pre_close_warning_run_async(uint32_t duration_ms, void (*on_complete)(void));

#ifdef __cplusplus
}
#endif