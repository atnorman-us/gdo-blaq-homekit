#include "homekit_notify.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "wifi.h"
#include <inttypes.h> 

#include "gdo.h"

#include "tasks.h"
#include "homekit_decl.h"
#include "homekit.h"

static const char* TAG = "test_main";

// ────────────────────────────────────────────────
//  Door-state monitoring / recovery tuning
// ────────────────────────────────────────────────
//
// Raw position is 0 (open) .. 10000 (closed) but sensor drift/noise means it
// may never hit the exact endpoints. Use tolerance bands instead of exact
// equality so a door that's physically open/closed doesn't get stuck
// reporting OPENING/CLOSING forever.
#define GDO_DOOR_POS_OPEN_THRESHOLD    200     // raw <= this  => treat as OPEN
#define GDO_DOOR_POS_CLOSED_THRESHOLD  9800    // raw >= this  => treat as CLOSED

// Longest a real door should ever be mid-transition, used only as a fallback
// before the library has measured actual open/close duration (gdo_status_t's
// open_ms/close_ms). Once measured, we time out at measured-duration + margin
// instead of guessing.
#define GDO_DOOR_TRANSIT_FALLBACK_MS   35000
#define GDO_DOOR_TRANSIT_MARGIN_MS     10000

// If we haven't seen *any* door-position frame in this long, the UART link
// itself is probably dead (not just "door is idle") - kick a resync.
#define GDO_LINK_STALE_TIMEOUT_MS      60000

#define GDO_WATCHDOG_PERIOD_MS         5000

// Last-known states for filtering duplicate notifications
static gdo_light_state_t       last_light       = GDO_LIGHT_STATE_MAX;
static gdo_lock_state_t        last_lock        = GDO_LOCK_STATE_MAX;
static gdo_door_state_t        last_door        = GDO_DOOR_STATE_MAX;
static gdo_obstruction_state_t last_obstruction = GDO_OBSTRUCTION_STATE_MAX;
static gdo_motion_state_t      last_motion      = GDO_MOTION_STATE_MAX;
//static gdo_learn_state_t       last_learn       = GDO_LEARN_STATE_MAX;

// Recovery/monitoring state
static volatile int64_t          s_last_status_event_ms = 0;
static volatile bool             s_door_in_transition   = false;
static volatile int64_t          s_transition_start_ms  = 0;
static volatile gdo_door_state_t s_transition_target    = GDO_DOOR_STATE_MAX;

static inline int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void gdo_event_handler(const gdo_status_t* status, gdo_cb_event_t event, void *arg)
{
    // Any event at all (light, lock, position, obstruction, ...) means the
    // serial link to the opener is alive. Used by the watchdog task below to
    // detect a fully dead link vs. a door that's just legitimately idle.
    s_last_status_event_ms = now_ms();

    switch (event) {

    case GDO_CB_EVENT_SYNCED:
        ESP_LOGI(TAG, "Synced: %s, protocol: %s",
                 status->synced ? "true" : "false",
                 gdo_protocol_type_to_string(status->protocol));

        if (status->protocol == GDO_PROTOCOL_SEC_PLUS_V2) {
            ESP_LOGI(TAG, "Client ID: %" PRIu32 ", Rolling code: %" PRIu32,
                     status->client_id, status->rolling_code);
        }

        if (!status->synced) {
            if (gdo_set_rolling_code(status->rolling_code + 100) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set rolling code");
            } else {
                ESP_LOGI(TAG, "Rolling code set to %" PRIu32 ", retrying sync",
                         status->rolling_code);
                gdo_sync();
            }
        }
        break;

    case GDO_CB_EVENT_LIGHT:
        if (status->light != last_light) {
            last_light = status->light;
            ESP_LOGI(TAG, "Light: %s", gdo_light_state_to_string(status->light));
            notify_homekit_light(status->light);
        }
        break;

    case GDO_CB_EVENT_LOCK:
        if (status->lock != last_lock) {
            last_lock = status->lock;
            ESP_LOGI(TAG, "Lock: %s", gdo_lock_state_to_string(status->lock));
            notify_homekit_current_lock(status->lock);
        }
        break;

case GDO_CB_EVENT_DOOR_POSITION: {

    uint32_t raw = status->door_position;
    if (raw > 10000) raw = 10000;

    ESP_LOGI(TAG,
             "Door raw: %s, raw=%" PRIu32 ", target=%" PRIu32 ", proto=%s",
             gdo_door_state_to_string(status->door),
             raw,
             (uint32_t)status->door_target,
             gdo_protocol_type_to_string(status->protocol));

    //
    // Do NOT infer anything until sync completes
    //
    if (!status->synced) {
        break;
    }

    gdo_door_state_t inferred = status->door;

    //
    // ────────────────────────────────────────────────
    //  SECURITY+ 1.0
    // ────────────────────────────────────────────────
    //
    if (status->protocol == GDO_PROTOCOL_SEC_PLUS_V1 ||
        status->protocol == GDO_PROTOCOL_SEC_PLUS_V1_WITH_SMART_PANEL) {

        if (status->door == GDO_DOOR_STATE_STOPPED ||
            status->door == GDO_DOOR_STATE_MAX) {

            if (raw <= GDO_DOOR_POS_OPEN_THRESHOLD)
                inferred = GDO_DOOR_STATE_OPEN;
            else if (raw >= GDO_DOOR_POS_CLOSED_THRESHOLD)
                inferred = GDO_DOOR_STATE_CLOSED;
        }

        if (inferred == GDO_DOOR_STATE_OPENING)
            notify_homekit_target_door_state_change(TGT_OPEN);
        else if (inferred == GDO_DOOR_STATE_CLOSING)
            notify_homekit_target_door_state_change(TGT_CLOSED);
    }

    //
    // ────────────────────────────────────────────────
    //  SECURITY+ 2.0
    // ────────────────────────────────────────────────
    //
//
// SECURITY+ 2.0 movement inference (raw may NOT change mid‑movement)
//
if (status->protocol == GDO_PROTOCOL_SEC_PLUS_V2) {

    static uint32_t last_raw    = raw;
    static uint32_t last_target = status->door_target;
    static gdo_door_state_t last_state = status->door;

    bool raw_changed    = (raw != last_raw);
    bool target_changed = (status->door_target != last_target);
    bool state_changed  = (status->door != last_state);

    last_raw    = raw;
    last_target = status->door_target;
    last_state  = status->door;

    //
    // Final OPEN/CLOSED detection
    //
    if (raw <= GDO_DOOR_POS_OPEN_THRESHOLD) {
        inferred = GDO_DOOR_STATE_OPEN;
    }
    else if (raw >= GDO_DOOR_POS_CLOSED_THRESHOLD) {
        inferred = GDO_DOOR_STATE_CLOSED;
    }
    else {

        //
        // Movement start triggered by target change
        //
        if (target_changed) {
            if (status->door_target == 0)
                inferred = GDO_DOOR_STATE_OPENING;
            else if (status->door_target == 10000)
                inferred = GDO_DOOR_STATE_CLOSING;
        }

        //
        // Movement start triggered by door state change
        //
        else if (state_changed) {
            if (status->door == GDO_DOOR_STATE_OPENING)
                inferred = GDO_DOOR_STATE_OPENING;
            else if (status->door == GDO_DOOR_STATE_CLOSING)
                inferred = GDO_DOOR_STATE_CLOSING;
        }

        //
        // Movement start triggered by raw change (if your opener ever sends mid‑movement frames)
        //
        else if (raw_changed) {
            if (status->door_target == 0)
                inferred = GDO_DOOR_STATE_OPENING;
            else if (status->door_target == 10000)
                inferred = GDO_DOOR_STATE_CLOSING;
        }
    }

    //
    // Drive HomeKit target from door_target
    //
    if (status->door_target == 0)
        notify_homekit_target_door_state_change(TGT_OPEN);
    else if (status->door_target == 10000)
        notify_homekit_target_door_state_change(TGT_CLOSED);
}

    //
    // ────────────────────────────────────────────────
    //  TRANSITION TRACKING (feeds the watchdog task)
    // ────────────────────────────────────────────────
    //
    if (inferred == GDO_DOOR_STATE_OPENING || inferred == GDO_DOOR_STATE_CLOSING) {
        if (!s_door_in_transition || s_transition_target != inferred) {
            s_door_in_transition  = true;
            s_transition_start_ms = now_ms();
            s_transition_target   = inferred;
        }
    } else {
        // We reached a resolved state (OPEN/CLOSED/STOPPED) on our own -
        // nothing stuck here, clear the watchdog's tracking.
        s_door_in_transition = false;
    }

    //
    // ────────────────────────────────────────────────
    //  HOMEKIT UPDATE (shared)
    // ────────────────────────────────────────────────
    //
    if (inferred != last_door) {
        last_door = inferred;
        notify_homekit_current_door_state_change(inferred);
    }

    if (status->light != last_light) {
        last_light = status->light;
        notify_homekit_light(status->light);
    }

    break;
}

    case GDO_CB_EVENT_OBSTRUCTION:
        if (status->obstruction != last_obstruction) {
            last_obstruction = status->obstruction;
            notify_homekit_obstruction(status->obstruction);
        }
        break;

    case GDO_CB_EVENT_MOTION:
        if (status->motion != last_motion) {
            last_motion = status->motion;
            ESP_LOGI(TAG, "Motion: %s", gdo_motion_state_to_string(status->motion));
            notify_homekit_motion(status->motion);
        }
        break;

    case GDO_CB_EVENT_BATTERY:
        ESP_LOGI(TAG, "Battery: %s", gdo_battery_state_to_string(status->battery));
        break;

    case GDO_CB_EVENT_BUTTON:
        ESP_LOGI(TAG, "Button: %s", gdo_button_state_to_string(status->button));
        break;

    case GDO_CB_EVENT_MOTOR:
        ESP_LOGI(TAG, "Motor: %s", gdo_motor_state_to_string(status->motor));
        break;

    case GDO_CB_EVENT_OPENINGS:
        ESP_LOGI(TAG, "Openings: %d", status->openings);
        break;

    case GDO_CB_EVENT_TTC:
        ESP_LOGI(TAG, "Time to close: %d", status->ttc_seconds);
        break;

    case GDO_CB_EVENT_PAIRED_DEVICES:
        ESP_LOGI(TAG,
                 "Paired devices: %d remotes, %d keypads, %d wall controls, %d accessories, %d total",
                 status->paired_devices.total_remotes,
                 status->paired_devices.total_keypads,
                 status->paired_devices.total_wall_controls,
                 status->paired_devices.total_accessories,
                 status->paired_devices.total_all);
        break;

    case GDO_CB_EVENT_OPEN_DURATION_MEASUREMENT:
        ESP_LOGI(TAG, "Measured open duration: %u ms", status->open_ms);
        break;

    case GDO_CB_EVENT_CLOSE_DURATION_MEASUREMENT:
        ESP_LOGI(TAG, "Measured close duration: %u ms", status->close_ms);
        break;

    default:
        ESP_LOGI(TAG, "Unknown event: %d", event);
        break;
    }
}


// ────────────────────────────────────────────────
//  Monitoring / recovery watchdog
// ────────────────────────────────────────────────
//
// Runs independently of the GDO callback, so it keeps working even if the
// serial link has gone completely silent.
static void gdo_watchdog_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(GDO_WATCHDOG_PERIOD_MS));
        int64_t now = now_ms();

        // 1) Stuck mid-transition: door has been OPENING/CLOSING far longer
        //    than it actually takes. Almost always means the "arrived" frame
        //    was dropped. Pull a fresh status directly (gdo_get_status() is a
        //    cheap read, unlike gdo_sync() which renegotiates the rolling
        //    code) and re-derive the real position ourselves.
        if (s_door_in_transition) {
            gdo_status_t status;
            esp_err_t err = gdo_get_status(&status);

            if (err == ESP_OK) {
                uint32_t elapsed = (uint32_t)(now - s_transition_start_ms);

                // Use the door's own measured travel time once the library
                // has learned it; otherwise fall back to a conservative
                // fixed ceiling.
                uint16_t measured_ms = (s_transition_target == GDO_DOOR_STATE_OPENING)
                                           ? status.open_ms
                                           : status.close_ms;
                uint32_t timeout_ms = measured_ms > 0
                                           ? (uint32_t)measured_ms + GDO_DOOR_TRANSIT_MARGIN_MS
                                           : GDO_DOOR_TRANSIT_FALLBACK_MS;

                if (elapsed > timeout_ms) {
                    uint32_t raw = (uint32_t)status.door_position;
                    if (raw > 10000) raw = 10000;

                    gdo_door_state_t resolved = GDO_DOOR_STATE_MAX;
                    if (raw <= GDO_DOOR_POS_OPEN_THRESHOLD)
                        resolved = GDO_DOOR_STATE_OPEN;
                    else if (raw >= GDO_DOOR_POS_CLOSED_THRESHOLD)
                        resolved = GDO_DOOR_STATE_CLOSED;

                    s_door_in_transition = false;

                    if (resolved != GDO_DOOR_STATE_MAX) {
                        // The door actually arrived - our own event handler
                        // just never got (or acted on) the frame saying so.
                        ESP_LOGW(TAG,
                                 "%s timed out after %" PRIu32 " ms (limit %" PRIu32
                                 " ms) - re-derived %s from raw=%" PRIu32,
                                 gdo_door_state_to_string(s_transition_target), elapsed,
                                 timeout_ms, gdo_door_state_to_string(resolved), raw);

                        if (resolved != last_door) {
                            last_door = resolved;
                            notify_homekit_current_door_state_change(resolved);
                        }
                    } else {
                        // Position is still genuinely mid-range - either the
                        // door is mechanically stuck or the link has stalled.
                        // Don't guess OPEN/CLOSED; report STOPPED honestly and
                        // request a real resync.
                        ESP_LOGW(TAG,
                                 "%s timed out after %" PRIu32 " ms, raw=%" PRIu32
                                 " still mid-travel - marking STOPPED, requesting resync",
                                 gdo_door_state_to_string(s_transition_target), elapsed, raw);

                        if (last_door != GDO_DOOR_STATE_STOPPED) {
                            last_door = GDO_DOOR_STATE_STOPPED;
                            notify_homekit_current_door_state_change(GDO_DOOR_STATE_STOPPED);
                        }
                        gdo_sync();
                    }
                }
            } else {
                ESP_LOGW(TAG, "gdo_get_status() failed (%d) during transition watchdog check", err);
            }
        }

        // 2) Fully silent link: no status frames of any kind in a long
        //    time. Distinct from #1 - this fires even while idle, so it
        //    catches a dead UART link that #1 alone wouldn't notice.
        if (s_last_status_event_ms != 0 &&
            (now - s_last_status_event_ms) > GDO_LINK_STALE_TIMEOUT_MS) {

            ESP_LOGW(TAG,
                     "No GDO status events in %" PRId64 " ms - requesting resync",
                     now - s_last_status_event_ms);

            gdo_sync();

            // Debounce: don't fire again every watchdog tick while waiting
            // for the link to recover.
            s_last_status_event_ms = now;
        }
    }
}

extern "C" void app_main(void)
{
    gdo_config_t gdo_conf;
    gdo_conf.invert_uart    = true;
    gdo_conf.obst_from_status = true;
    gdo_conf.uart_num       = UART_NUM_1;
    gdo_conf.uart_tx_pin    = GPIO_NUM_1;
    gdo_conf.uart_rx_pin    = GPIO_NUM_2;
    gdo_conf.obst_in_pin    = GPIO_NUM_5;

    ESP_ERROR_CHECK(gdo_init(&gdo_conf));
    ESP_ERROR_CHECK(gdo_start(gdo_event_handler, NULL));

    xTaskCreate(homekit_task_entry,
                HOMEKIT_TASK_NAME,
                HOMEKIT_TASK_STK_SZ,
                NULL,
                HOMEKIT_TASK_PRIO,
                NULL);

    xTaskCreate(gdo_watchdog_task,
                "gdo_watchdog",
                3072,
                NULL,
                tskIDLE_PRIORITY + 2,
                NULL);

    ESP_LOGI(TAG, "GDO started!");
}
