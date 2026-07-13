#include "homekit_notify.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_err.h"
#include "wifi.h"
#include <inttypes.h> 

#include "gdo.h"

#include "tasks.h"
#include "homekit_decl.h"
#include "homekit.h"
#include "diag_webserver.h"

// Defined in homekit.cpp - creates the HomeKit notification queue early,
// before gdo_start() so no GDO event can fire before it exists.
extern "C" void homekit_notif_queue_init(void);

// notify_homekit_learn is defined in homekit.cpp but isn't declared in
// homekit.h or homekit_notify.h in this codebase - declared here as a
// stopgap. Worth adding it to homekit.h properly alongside the other
// notify_homekit_* declarations so future callers don't hit this too.
void notify_homekit_learn(gdo_learn_state_t learn);

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

// If we haven't seen *any* GDO event (of any kind) in this long, the UART
// link itself is probably dead (not just "door is idle") - kick a resync.
//
// 5 minutes, not 60s: confirmed via a real Sec+2.0 capture that this
// protocol doesn't send periodic idle status frames the way Sec+1.0 with
// smart panel does (which chatters roughly once a second even when
// nothing's happening). On that V2 capture, 61 seconds of total silence
// was completely normal idle behavior, not a stalled link - a 60s
// threshold was forcing an unnecessary gdo_sync() during ordinary idle
// periods. 5 minutes gives real margin above that while still catching a
// genuinely dead link in a reasonable time.
#define GDO_LINK_STALE_TIMEOUT_MS      300000

#define GDO_WATCHDOG_PERIOD_MS         5000

// Last-known states for filtering duplicate notifications
static gdo_light_state_t       last_light       = GDO_LIGHT_STATE_MAX;
static gdo_lock_state_t        last_lock        = GDO_LOCK_STATE_MAX;
static gdo_door_state_t        last_door        = GDO_DOOR_STATE_MAX;
static gdo_obstruction_state_t last_obstruction = GDO_OBSTRUCTION_STATE_MAX;
static gdo_motion_state_t      last_motion      = GDO_MOTION_STATE_MAX;

// Tracks consecutive sync failures within this boot, so the rolling-code
// retry jump can scale up instead of always adding a fixed +100. Reset to
// 0 on a successful sync.
static uint32_t s_sync_retry_count = 0;
static gdo_learn_state_t       last_learn       = GDO_LEARN_STATE_MAX;

// Signaled once GDO sync genuinely completes. Lets other code (e.g.
// homekit.cpp deciding whether to show the Learn Mode tile) block until the
// real protocol is known, instead of reading gdo_get_status() before sync
// has had any chance to run.
static SemaphoreHandle_t s_gdo_synced_sem = nullptr;
static volatile int64_t          s_last_status_event_ms = 0;
static volatile bool             s_door_in_transition   = false;
static volatile int64_t          s_transition_start_ms  = 0;
static volatile gdo_door_state_t s_transition_target    = GDO_DOOR_STATE_MAX;

static inline int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// Shared by the normal GDO_CB_EVENT_DOOR_POSITION path and the sync-complete
// catch-up call below. Needed because a DOOR_POSITION event can legitimately
// arrive a moment *before* status->synced flips true - that event used to be
// silently dropped (logged, but never recorded into last_door), and since
// gdolib only re-sends DOOR_POSITION on a genuine state *change*, a door that
// doesn't move again afterward left last_door permanently stuck at its
// GDO_DOOR_STATE_MAX startup sentinel for the rest of the session. Confirmed
// via a real capture: "Door raw: Open" logged at 59.57s, sync completed at
// 60.92s, and /status still showed "door":"null" 224 seconds later since the
// door never moved again to trigger a fresh event.
static void process_door_position(const gdo_status_t *status)
{
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
        return;
    }

    gdo_door_state_t inferred = status->door;

    //
    // ────────────────────────────────────────────────
    //  SECURITY+ 1.0
    // ────────────────────────────────────────────────
    //
    if (status->protocol == GDO_PROTOCOL_SEC_PLUS_V1 ||
        status->protocol == GDO_PROTOCOL_SEC_PLUS_V1_WITH_SMART_PANEL) {

        // Only fall back to raw position when the protocol has never told
        // us a state at all (GDO_DOOR_STATE_MAX, e.g. right after sync).
        // Do NOT do this for STOPPED: raw position can be stale on this
        // protocol (this device only sends a fresh position at the end of
        // a full open/closed cycle, not while genuinely mid-travel), so
        // guessing OPEN/CLOSED off it while STOPPED can misreport a
        // door that's actually stopped halfway as fully OPEN. Trust
        // STOPPED at face value - HomeKit has a real "Stopped" current
        // door state value for exactly this.
        if (status->door == GDO_DOOR_STATE_MAX) {
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
    // gdolib forwards status->door directly and immediately to HomeKit
    // itself (see update_door_state() in gdo.c) - it's the raw,
    // undecorated value decoded straight off the wire, not something we
    // need to re-derive. door_position isn't independent wire data
    // either: gdolib sets it to 0/10000 in the same call that sets
    // status->door to OPEN/CLOSED, and dead-reckons it via a timer during
    // OPENING/CLOSING using previously measured open_ms/close_ms - so
    // checking raw against a threshold here was just re-deriving what
    // status->door already says, through a value that originates from
    // status->door in the first place. Trust it directly, matching the
    // V1 approach above - only fall back to raw position when the
    // protocol has told us nothing at all (GDO_DOOR_STATE_MAX).
    if (status->protocol == GDO_PROTOCOL_SEC_PLUS_V2) {
        if (status->door == GDO_DOOR_STATE_MAX) {
            if (raw <= GDO_DOOR_POS_OPEN_THRESHOLD)
                inferred = GDO_DOOR_STATE_OPEN;
            else if (raw >= GDO_DOOR_POS_CLOSED_THRESHOLD)
                inferred = GDO_DOOR_STATE_CLOSED;
        }

        // Drive HomeKit target from door_target
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
        // If we're jumping directly between two resolved states (Closed <->
        // Open) without ever having reported an intermediate Opening/Closing,
        // the opener likely only sent a single status frame reflecting the
        // final position - confirmed via real Sec+2.0 captures where one
        // transition showed "Closing" mid-travel and another, on the same
        // device in the same session, went straight from Closed to Open
        // with zero transitional frames. We can't report the transition in
        // real time (we didn't know about it until it was already done),
        // but we can at least push a brief transitional state before the
        // final one so HomeKit shows *something* changed rather than an
        // unexplained instant flip.
        if (last_door == GDO_DOOR_STATE_CLOSED && inferred == GDO_DOOR_STATE_OPEN) {
            notify_homekit_current_door_state_change(GDO_DOOR_STATE_OPENING);
        } else if (last_door == GDO_DOOR_STATE_OPEN && inferred == GDO_DOOR_STATE_CLOSED) {
            notify_homekit_current_door_state_change(GDO_DOOR_STATE_CLOSING);
        }

        last_door = inferred;
        notify_homekit_current_door_state_change(inferred);

        // Keep Target in sync whenever Current resolves to a definite
        // OPEN/CLOSED. This matters most right after boot: the HAP
        // service is created with Target hardcoded to OPEN every time
        // (see hap_serv_garage_door_opener_create() in homekit.cpp),
        // regardless of the door's actual state, and we only otherwise
        // push Target during an active OPENING/CLOSING transition - so a
        // door that's already Closed at boot (or any other path that
        // resolves Current without going through that transition) would
        // leave Target stuck at Open forever. Home app renders that
        // Target/Current mismatch as a phantom "Opening..." even though
        // Current correctly says Closed.
        //
        // Re-enabled after ruling this out as the cause of a door
        // open/close/open cycling incident - that was actually a crash
        // loop in diag_webserver.cpp hammering gdo_init()/gdo_start() on
        // every reboot (fixed separately). Re-tested clean before
        // re-enabling this.
        if (inferred == GDO_DOOR_STATE_OPEN) {
            notify_homekit_target_door_state_change(TGT_OPEN);
        } else if (inferred == GDO_DOOR_STATE_CLOSED) {
            notify_homekit_target_door_state_change(TGT_CLOSED);
        } else if (inferred == GDO_DOOR_STATE_STOPPED) {
            // HomeKit's TargetDoorState only supports Open/Closed - there's
            // no "stopped" option. Leaving Target at whatever it was before
            // the stop (still pointing at the original direction of travel,
            // e.g. Open if the door was mid-opening) makes HomeKit read the
            // stop as an unconfirmed move still in progress rather than a
            // settled state - confirmed via a real capture where stopping
            // the door mid-open left Target stuck at Open while Current
            // correctly said Stopped, and Home app showed "Open" instead of
            // "Stopped". Pick whichever endpoint the door is actually
            // closer to instead of leaving a stale value.
            //
            // Guard against status->door_position genuinely being unknown
            // (gdolib's -1 sentinel, e.g. right after a fresh sync before
            // any real position data exists) - our unsigned raw clamp turns
            // that into raw=10000, which would be misread as "definitely
            // near Closed" even though it isn't a real position at all.
            // Confirmed via a real boot-time capture showing exactly this:
            // "target=4294967295" (int32_t -1 as unsigned) right as sync
            // completed. Skip the guess entirely rather than push a target
            // based on data that was never a real reading.
            if (status->door_position >= 0) {
                if (raw <= 5000) {
                    notify_homekit_target_door_state_change(TGT_OPEN);
                } else {
                    notify_homekit_target_door_state_change(TGT_CLOSED);
                }
            }
        }
    }

    if (status->light != last_light) {
        last_light = status->light;
        notify_homekit_light(status->light);
    }
}

// Persists the Security+ 2.0 client_id/rolling_code across reboots so a
// normal restart can seed sync from close to where the last session left
// off, instead of gdolib's hardcoded defaults every single time.
//
// Confirmed the need for this via two consecutive real boots that needed
// the IDENTICAL 5-round rolling-code retry sequence
// (21->121->342->763->1584->3205, landing at the same final value both
// times) - proof the starting point was completely disconnected from
// wherever the real opener's counter had actually been left by real-world
// use between boots. Without persistence, every reboot re-discovers the
// gap from scratch via trial and error, no matter how recently the device
// last synced successfully.
#define GDO_NVS_NAMESPACE      "gdo_rolling"
#define GDO_NVS_KEY_CLIENT_ID  "client_id"
#define GDO_NVS_KEY_ROLLING    "rolling_code"

// Called once, early in app_main(), before gdo_start() - seeds gdolib
// with the last known-good values if any are saved. Safe to call even if
// nothing has ever been saved (first boot, or NVS was erased): NVS simply
// won't have the namespace/keys yet, which is treated as "nothing to
// seed," not an error.
static void gdo_load_persisted_rolling_state(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(GDO_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No persisted rolling-code state found (%s) - using gdolib defaults",
                 esp_err_to_name(err));
        return;
    }

    uint32_t client_id = 0;
    uint32_t rolling_code = 0;
    bool have_client_id = (nvs_get_u32(handle, GDO_NVS_KEY_CLIENT_ID, &client_id) == ESP_OK);
    bool have_rolling_code = (nvs_get_u32(handle, GDO_NVS_KEY_ROLLING, &rolling_code) == ESP_OK);
    nvs_close(handle);

    if (have_client_id) {
        if (gdo_set_client_id(client_id) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to seed persisted client ID");
        }
    }

    if (have_rolling_code) {
        // Seeded at exactly the last saved value, no artificial margin -
        // if the opener's real counter has moved further since the last
        // save (likely, given any real-world use in between), the
        // existing scaled-jump retry logic already closes that gap
        // efficiently in a few rounds. Guessing at a speculative offset
        // here would add complexity without evidence for what margin is
        // actually safe on this protocol.
        if (gdo_set_rolling_code(rolling_code) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to seed persisted rolling code");
        } else {
            ESP_LOGI(TAG, "Seeded client ID %" PRIu32 ", rolling code %" PRIu32 " from NVS",
                     client_id, rolling_code);
        }
    }
}

// Called after a successful sync (see GDO_CB_EVENT_SYNCED). Deliberately
// NOT called on every rolling-code change during normal operation (every
// door/light/lock command increments it) - NVS flash has a limited
// write-cycle lifetime, and sync only happens a handful of times per
// boot, keeping write frequency low while still delivering the real
// benefit: the next reboot starts from here instead of from scratch.
static void gdo_save_rolling_state(uint32_t client_id, uint32_t rolling_code)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(GDO_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for rolling-code save: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_u32(handle, GDO_NVS_KEY_CLIENT_ID, client_id);
    nvs_set_u32(handle, GDO_NVS_KEY_ROLLING, rolling_code);
    esp_err_t commit_err = nvs_commit(handle);
    nvs_close(handle);

    if (commit_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to commit rolling-code save: %s", esp_err_to_name(commit_err));
    } else {
        ESP_LOGI(TAG, "Saved client ID %" PRIu32 ", rolling code %" PRIu32 " to NVS",
                 client_id, rolling_code);
    }
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
            // Scale the jump with consecutive failures instead of a fixed
            // +100 every time. A real capture needed 5 retry rounds to
            // close a ~500-unit rolling-code gap (likely from heavy real
            // opener usage - remote/wall button/keypad - advancing the
            // opener's counter well past what a single +100 step could
            // catch up to in one round), taking over a minute total since
            // each round re-pays the full ~12-13s detection sequence.
            // Doubling per consecutive failure closes a large gap in far
            // fewer rounds while still starting conservative (+100) for
            // the common case of a small drift. Capped at +1600 to avoid
            // a wild overshoot on a single retry.
            s_sync_retry_count++;
            uint32_t shift = (s_sync_retry_count > 4) ? 4 : (s_sync_retry_count - 1);
            uint32_t jump = 100u << shift;

            if (gdo_set_rolling_code(status->rolling_code + jump) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set rolling code");
            } else {
                ESP_LOGI(TAG, "Rolling code set to %" PRIu32 " (+%" PRIu32 ", attempt %" PRIu32 "), retrying sync",
                         status->rolling_code, jump, s_sync_retry_count);
                gdo_sync();
            }
        } else {
            s_sync_retry_count = 0;
            if (s_gdo_synced_sem) {
                xSemaphoreGive(s_gdo_synced_sem);
            }

            // Persist for next boot - see gdo_save_rolling_state() comment.
            // V2-only: client_id/rolling_code are meaningless on V1, which
            // doesn't use this rolling-code scheme at all.
            if (status->protocol == GDO_PROTOCOL_SEC_PLUS_V2) {
                gdo_save_rolling_state(status->client_id, status->rolling_code);
            }

            // Catch-up: a DOOR_POSITION event can legitimately arrive a
            // moment before synced flips true, and that event's own call
            // to process_door_position() would have bailed out early (it
            // requires synced). Since gdolib only re-sends DOOR_POSITION on
            // a genuine state *change*, a door that doesn't move again
            // afterward would leave last_door stuck at its startup
            // sentinel indefinitely. Re-run it now that we know sync is
            // genuinely done, using the same status snapshot - cheap and
            // idempotent if nothing was actually missed.
            process_door_position(status);
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
    process_door_position(status);
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

        // Motor turning on is a strong, direct signal that a transition
        // has started - it arrives well before (often several real
        // seconds before) any Door raw: Opening/Closing frame, if one
        // ever comes at all. Confirmed via real logs: Motor:On preceded
        // the final resolved frame by ~8-14s in transitions that produced
        // zero telemetry in between. Direction is inferable with real
        // confidence from the last confirmed resolved state - a door
        // that was Closed can only be starting to open, and vice versa.
        // Skip inference if the last known state was Stopped/unknown/
        // already mid-transition, where direction genuinely isn't
        // knowable from this signal alone.
        //
        // KNOWN RISK, ACCEPTED FOR NOW: a real incident showed the door
        // displaying "Closing" while it was actually Open, persisting for
        // 45+ seconds - well beyond the ~25-32s the stuck-transition
        // watchdog below should take to self-correct (measured travel
        // time + 10s margin, checked every 5s). No log of that specific
        // incident exists, so the exact failure mechanism in the recovery
        // path is still unknown. Re-enabled at the user's request - they
        // have an independent way (cameras) to verify real door state
        // while this is being evaluated further. If this recurs, capture
        // a log covering the incident immediately; that's what's actually
        // needed to diagnose it properly rather than guessing again.
        if (status->motor == GDO_MOTOR_STATE_ON) {
            gdo_door_state_t motor_inferred = GDO_DOOR_STATE_MAX;

            if (last_door == GDO_DOOR_STATE_CLOSED) {
                motor_inferred = GDO_DOOR_STATE_OPENING;
            } else if (last_door == GDO_DOOR_STATE_OPEN) {
                motor_inferred = GDO_DOOR_STATE_CLOSING;
            }

            if (motor_inferred != GDO_DOOR_STATE_MAX && motor_inferred != last_door) {
                last_door = motor_inferred;
                notify_homekit_current_door_state_change(motor_inferred);

                if (motor_inferred == GDO_DOOR_STATE_OPENING) {
                    notify_homekit_target_door_state_change(TGT_OPEN);
                } else {
                    notify_homekit_target_door_state_change(TGT_CLOSED);
                }

                // Start transition tracking now, at the earliest real
                // signal we have, instead of waiting for a later
                // DOOR_POSITION event (if one ever arrives) - gives the
                // stuck-transition watchdog and status-refresh polling an
                // accurate start time too.
                s_door_in_transition  = true;
                s_transition_start_ms = now_ms();
                s_transition_target   = motor_inferred;
            }
        }
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

    case GDO_CB_EVENT_LEARN:
        if (status->learn != last_learn) {
            last_learn = status->learn;
            ESP_LOGI(TAG, "Learn: %s", gdo_learn_state_to_string(status->learn));
            notify_homekit_learn(status->learn);
        }
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
            // Actively ask the opener for a fresh status update while a
            // transition is known to be in progress, rather than only
            // waiting for it to volunteer one. Confirmed via real captures
            // that some transitions produce zero telemetry between the
            // start and end frames, leaving HomeKit showing stale state
            // for the door's full ~10-15s travel time. This can't fix that
            // if the opener genuinely never answers an on-demand request,
            // but it's worth trying - requires gdo_refresh_status() to be
            // added to gdolib (see gdo_refresh_status_patch.md); this call
            // is a no-op error (logged, harmless) until that's applied.
            esp_err_t refresh_err = gdo_refresh_status();
            if (refresh_err == ESP_OK) {
                ESP_LOGD(TAG, "Requesting status refresh during transition");
            } else {
                ESP_LOGD(TAG, "Status refresh request failed: %s", esp_err_to_name(refresh_err));
            }

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

                    // gdolib's status.door is the authoritative signal for
                    // every protocol (see gdo_event_handler's V1/V2
                    // handling above - door_position turned out to be
                    // derived from door state by gdolib itself, not
                    // independent wire data, so there's no reason to treat
                    // V1/V2 differently here anymore). Trust OPEN/CLOSED/
                    // STOPPED directly whenever the protocol has already
                    // reported one. Only fall back to raw position when
                    // the protocol has told us nothing at all (MAX).
                    if (status.door == GDO_DOOR_STATE_OPEN ||
                        status.door == GDO_DOOR_STATE_CLOSED ||
                        status.door == GDO_DOOR_STATE_STOPPED) {
                        resolved = status.door;
                    } else if (status.door == GDO_DOOR_STATE_MAX) {
                        if (raw <= GDO_DOOR_POS_OPEN_THRESHOLD) {
                            resolved = GDO_DOOR_STATE_OPEN;
                        } else if (raw >= GDO_DOOR_POS_CLOSED_THRESHOLD) {
                            resolved = GDO_DOOR_STATE_CLOSED;
                        }
                    }

                    s_door_in_transition = false;

                    if (resolved != GDO_DOOR_STATE_MAX) {
                        // The door actually arrived - our own event handler
                        // just never got (or acted on) the frame saying so.
                        bool from_protocol_state = (status.door == resolved);
                        ESP_LOGW(TAG,
                                 "%s timed out after %" PRIu32 " ms (limit %" PRIu32
                                 " ms) - re-derived %s from %s (raw=%" PRIu32 ")",
                                 gdo_door_state_to_string(s_transition_target), elapsed,
                                 timeout_ms, gdo_door_state_to_string(resolved),
                                 from_protocol_state ? "status.door" : "raw position",
                                 raw);

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

// ────────────────────────────────────────────────
//  Diagnostics accessor
// ────────────────────────────────────────────────
//
// Read-only snapshot of the last-known states above, for the diagnostics
// web server (see diag_webserver.cpp). Kept here rather than exposing the
// statics directly so this file stays the single owner of this state.
extern "C" void gdo_diag_get_last_states(gdo_door_state_t *door,
                                          gdo_light_state_t *light,
                                          gdo_lock_state_t *lock,
                                          gdo_obstruction_state_t *obstruction,
                                          gdo_motion_state_t *motion)
{
    if (door)        *door = last_door;
    if (light)       *light = last_light;
    if (lock)        *lock = last_lock;
    if (obstruction) *obstruction = last_obstruction;
    if (motion)      *motion = last_motion;
}

// Blocks until GDO sync completes or timeout_ms elapses, whichever is
// first. Returns true if sync completed within the timeout. Used by
// homekit.cpp to know the real protocol before it has to finalize the HAP
// accessory database (which can't be changed after hap_start()).
extern "C" bool gdo_wait_for_sync(uint32_t timeout_ms)
{
    if (!s_gdo_synced_sem) {
        return false;
    }
    return xSemaphoreTake(s_gdo_synced_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

extern "C" void app_main(void)
{
    // Required before any nvs_open() call - this is the top-level entry
    // point, nothing else runs before app_main() that would have already
    // initialized NVS. Standard ESP-IDF boilerplate: reinit on the two
    // recoverable error cases (partition truncated/corrupt or found from
    // an older NVS format), otherwise fail loudly via ESP_ERROR_CHECK.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    gdo_config_t gdo_conf;
    gdo_conf.invert_uart    = true;
    gdo_conf.obst_from_status = true;
    gdo_conf.uart_num       = UART_NUM_1;
    gdo_conf.uart_tx_pin    = GPIO_NUM_1;
    gdo_conf.uart_rx_pin    = GPIO_NUM_2;
    gdo_conf.obst_in_pin    = GPIO_NUM_5;

    s_gdo_synced_sem = xSemaphoreCreateBinary();

    // Defined in homekit.cpp - must run before gdo_start() so the
    // notification queue exists before any GDO event callback can fire.
    // Previously this only happened inside homekit_task_entry (a separate
    // task not guaranteed to have run yet), which silently dropped the
    // very first boot-time Light/Lock/Door values.
    homekit_notif_queue_init();

    ESP_ERROR_CHECK(gdo_init(&gdo_conf));

    // Seed any persisted rolling-code state before starting sync - must
    // happen after gdo_init() (so g_status exists) and before gdo_start()
    // (which internally kicks off gdo_sync() - gdo_set_rolling_code()/
    // gdo_set_client_id() both refuse once synced is true).
    gdo_load_persisted_rolling_state();

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

    diag_webserver_start();

    ESP_LOGI(TAG, "GDO started!");
}