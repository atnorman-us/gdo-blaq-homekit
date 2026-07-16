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
#include "pre_close_warning.h"

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

// If the motor turns on but raw position hasn't moved a meaningful amount
// within this window, treat it as a refused/aborted move (e.g. opener's
// own safety circuit vetoed the close because the beam was already broken
// at the exact moment the command arrived) rather than a genuinely slow
// transit. Confirmed in the field: MOTOR_ON fired once, raw stayed pinned
// at 0 for the door's ENTIRE ~38-second wait before the old fallback
// timeout finally caught it - a real transit changes raw within a couple
// seconds (see successful cycles elsewhere in these logs), so 8s is
// generous margin, not a hair-trigger.
#define GDO_DOOR_STALL_CHECK_MS        8000
#define GDO_DOOR_STALL_MOVEMENT_MIN    200

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

// If Obstructed stays confirmed this long with zero fresh readings to
// back it up, treat it as stale and auto-clear. gdolib doesn't send a
// steady heartbeat of OBST_1/OBST_2 frames while idle - it only reports
// around motion/status activity - so a real obstruction event can be
// confirmed correctly and then never followed by a "Clear" reading if
// the door settles and traffic quiets down, leaving HomeKit permanently
// stuck showing Obstructed. Confirmed in the field: walked through the
// beam during a real close, door correctly auto-reversed and reopened,
// obstruction correctly confirmed - then no further OBST_1/OBST_2
// traffic arrived at all, and Home app never cleared on its own.
#define GDO_OBSTRUCTION_STALE_TIMEOUT_MS  30000

// If a close/open request has gone unanswered this long - status.door_target
// disagrees with status.door, and s_door_in_transition never became true
// because no motion was ever confirmed - treat the request as refused and
// revert HomeKit's target back to reality. Confirmed in the field: opener
// refused to start closing (beam broken at the exact moment the close
// command arrived), motor never engaged, and HomeKit was left showing a
// permanent "Closing" spinner with no self-correction, since the existing
// stuck-transition watchdog is entirely gated on s_door_in_transition,
// which requires motion to have started at all.
#define GDO_MOTOR_ENGAGE_TIMEOUT_MS  8000

// Auto-close: if the door has been continuously OPEN (confirmed, not
// mid-transition) this long, close it automatically - same warning path
// as any other close, and skipped entirely if the last known obstruction
// reading isn't Clear. Runtime-configurable via NVS (see
// auto_close_settings_load()/gdo_set_auto_close_timeout_ms() below) - this
// is only the fallback used the first time the device ever boots, before
// any value has been saved.
#define GDO_AUTO_CLOSE_TIMEOUT_DEFAULT_MS  (60 * 60 * 1000)

// Last-known states for filtering duplicate notifications
static gdo_light_state_t       last_light       = GDO_LIGHT_STATE_MAX;
static gdo_lock_state_t        last_lock        = GDO_LOCK_STATE_MAX;
static gdo_door_state_t        last_door        = GDO_DOOR_STATE_MAX;
static gdo_obstruction_state_t last_obstruction = GDO_OBSTRUCTION_STATE_MAX;
static gdo_motion_state_t      last_motion      = GDO_MOTION_STATE_MAX;
static esp_timer_handle_t      obstruction_stale_timer = NULL;

// Auto-close tracking. s_door_open_since_ms is 0 whenever the door isn't
// confirmed OPEN right now; set exclusively through set_last_door_state()
// below so every place that updates last_door keeps this in sync too,
// rather than needing six separate call sites to remember to.
static volatile int64_t s_door_open_since_ms   = 0;
static volatile bool    s_auto_close_triggered = false;
static volatile uint32_t s_auto_close_timeout_ms = GDO_AUTO_CLOSE_TIMEOUT_DEFAULT_MS;

// Off by default - auto-close is a real safety-relevant behavior change
// (the door will move on its own with nobody having asked it to, right
// now), not something that should silently start happening just because
// firmware got updated. When first enabled with no saved timeout yet,
// s_auto_close_timeout_ms is already sitting at its 60-minute default
// above, so enabling it needs no extra "populate the default" logic.
static volatile bool s_auto_close_enabled = false;

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

// Tracks a commanded target (gdo_door_close()/gdo_door_open() was called,
// so gdolib's own status.door_target no longer matches status.door) that
// s_door_in_transition never picked up because no motion was ever
// confirmed at all - see GDO_MOTOR_ENGAGE_TIMEOUT_MS below for why this
// exists as a separate watchdog from the one above.
static volatile gdo_door_state_t s_pending_target_state    = GDO_DOOR_STATE_MAX;
static volatile int64_t          s_pending_target_start_ms = 0;
static volatile uint32_t         s_transition_start_raw    = 0;

static inline int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// Every write to last_door goes through here instead of a bare assignment,
// so auto-close's "how long has it been continuously OPEN" tracking stays
// correct no matter which of the six call sites in this file changed the
// state - a door leaving OPEN (for any reason: closing, obstruction,
// re-sync, watchdog correction, ...) always resets the clock, and a door
// arriving at OPEN always starts it, with no per-call-site bookkeeping to
// remember or forget.
static void set_last_door_state(gdo_door_state_t new_state)
{
    if (new_state == GDO_DOOR_STATE_OPEN && last_door != GDO_DOOR_STATE_OPEN) {
        s_door_open_since_ms   = now_ms();
        s_auto_close_triggered = false;
    } else if (new_state != GDO_DOOR_STATE_OPEN && last_door == GDO_DOOR_STATE_OPEN) {
        s_door_open_since_ms   = 0;
        s_auto_close_triggered = false;
    }
    last_door = new_state;
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
            s_transition_start_raw = raw;
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

        set_last_door_state(inferred);
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

// Separate namespace from the rolling-code state above - unrelated data,
// no reason to share a namespace or invalidate one by erasing the other.
#define GDO_SETTINGS_NVS_NAMESPACE      "gdo_settings"
#define GDO_SETTINGS_NVS_KEY_AUTOCLOSE  "auto_close_ms"
#define GDO_SETTINGS_NVS_KEY_AC_ENABLED "auto_close_en"

// Called once, early in app_main() - loads a previously saved auto-close
// timeout if one exists; otherwise s_auto_close_timeout_ms stays at its
// GDO_AUTO_CLOSE_TIMEOUT_DEFAULT_MS initializer (first boot, or NVS erased).
static void auto_close_settings_load(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(GDO_SETTINGS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved auto-close setting (%s) - using default (%" PRIu32 " ms)",
                 esp_err_to_name(err), (uint32_t)s_auto_close_timeout_ms);
        return;
    }

    uint32_t saved_ms = 0;
    if (nvs_get_u32(handle, GDO_SETTINGS_NVS_KEY_AUTOCLOSE, &saved_ms) == ESP_OK) {
        s_auto_close_timeout_ms = saved_ms;
        ESP_LOGI(TAG, "Loaded auto-close timeout from NVS: %" PRIu32 " ms", saved_ms);
    }

    uint8_t saved_enabled = 0;
    if (nvs_get_u8(handle, GDO_SETTINGS_NVS_KEY_AC_ENABLED, &saved_enabled) == ESP_OK) {
        s_auto_close_enabled = (saved_enabled != 0);
        ESP_LOGI(TAG, "Loaded auto-close enabled state from NVS: %s",
                 s_auto_close_enabled ? "enabled" : "disabled");
    }
    nvs_close(handle);
}

// Updates the in-memory timeout AND persists it, so it survives a reboot.
// Not static, and deliberately takes/returns plain types rather than
// anything GDO-library-specific - this is the function a future HTTP
// config handler (e.g. in diag_webserver.cpp) should call directly to let
// the auto-close duration be changed without a reflash. Needs a forward
// declaration in whatever header that file includes to call it - not
// wired up yet since diag_webserver.cpp hasn't been shared for this.
extern "C" esp_err_t gdo_set_auto_close_timeout_ms(uint32_t timeout_ms)
{
    s_auto_close_timeout_ms = timeout_ms;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(GDO_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Auto-close timeout updated in memory (%" PRIu32
                 " ms) but failed to open NVS to persist it: %s",
                 timeout_ms, esp_err_to_name(err));
        return err;
    }

    nvs_set_u32(handle, GDO_SETTINGS_NVS_KEY_AUTOCLOSE, timeout_ms);
    esp_err_t commit_err = nvs_commit(handle);
    nvs_close(handle);

    if (commit_err == ESP_OK) {
        ESP_LOGI(TAG, "Auto-close timeout set to %" PRIu32 " ms and saved to NVS", timeout_ms);
    } else {
        ESP_LOGW(TAG, "Auto-close timeout updated in memory but failed to save: %s",
                 esp_err_to_name(commit_err));
    }
    return commit_err;
}

// Read-only accessor, e.g. for a future HTTP config page to show the
// current value.
extern "C" uint32_t gdo_get_auto_close_timeout_ms(void)
{
    return s_auto_close_timeout_ms;
}

// Same pattern as gdo_set_auto_close_timeout_ms() - updates in-memory
// state and persists it. This is the toggle: auto-close is entirely
// gated on this being true, checked first in the watchdog before it even
// looks at door-open duration.
extern "C" esp_err_t gdo_set_auto_close_enabled(bool enabled)
{
    s_auto_close_enabled = enabled;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(GDO_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Auto-close %s in memory but failed to open NVS to persist it: %s",
                 enabled ? "enabled" : "disabled", esp_err_to_name(err));
        return err;
    }

    nvs_set_u8(handle, GDO_SETTINGS_NVS_KEY_AC_ENABLED, enabled ? 1 : 0);
    esp_err_t commit_err = nvs_commit(handle);
    nvs_close(handle);

    if (commit_err == ESP_OK) {
        ESP_LOGI(TAG, "Auto-close %s and saved to NVS", enabled ? "enabled" : "disabled");
    } else {
        ESP_LOGW(TAG, "Auto-close %s in memory but failed to save: %s",
                 enabled ? "enabled" : "disabled", esp_err_to_name(commit_err));
    }
    return commit_err;
}

extern "C" bool gdo_get_auto_close_enabled(void)
{
    return s_auto_close_enabled;
}

// Milliseconds remaining until auto-close would fire, or -1 if not
// currently counting down at all (disabled, door isn't confirmed open, or
// the warning/close sequence has already started for this open-cycle -
// s_auto_close_triggered is only cleared again once the door leaves and
// re-enters OPEN, see set_last_door_state()). For a dashboard countdown.
extern "C" int64_t gdo_get_auto_close_remaining_ms(void)
{
    if (!s_auto_close_enabled || s_door_open_since_ms == 0 || s_auto_close_triggered) {
        return -1;
    }
    int64_t elapsed = now_ms() - s_door_open_since_ms;
    int64_t remaining = (int64_t)s_auto_close_timeout_ms - elapsed;
    return remaining > 0 ? remaining : 0;
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
            // Debounce "Detected" only - a single corrupted UART frame
            // (this link logs frequent "RX data signature error" - see
            // gdo_event_handler's RX path) can misdecode into a spurious
            // obstruction bit. Confirmed via a real report: Home app
            // showed Obstructed with the door fully closed and physically
            // clear, no logging existed to see what triggered it. Require
            // the SAME detected reading to show up on two consecutive
            // OBSTRUCTION events before trusting it and pushing to
            // HomeKit - a real interruption re-reports continuously, so
            // this costs negligible real-world detection latency. Never
            // debounce "Clear" - there's no safety cost to clearing
            // faster, and it self-corrects a false Detected quickly.
            static gdo_obstruction_state_t s_pending_obstruction = GDO_OBSTRUCTION_STATE_MAX;

            if (status->obstruction == GDO_OBSTRUCTION_STATE_CLEAR) {
                s_pending_obstruction = GDO_OBSTRUCTION_STATE_MAX;
                last_obstruction = status->obstruction;
                ESP_LOGI(TAG, "Obstruction: %s",
                         gdo_obstruction_state_to_string(status->obstruction));
                notify_homekit_obstruction(status->obstruction);
                esp_timer_stop(obstruction_stale_timer); // cancel - no need to auto-clear what's already clear
            } else if (s_pending_obstruction == status->obstruction) {
                // Second consecutive matching reading - trust it.
                s_pending_obstruction = GDO_OBSTRUCTION_STATE_MAX;
                last_obstruction = status->obstruction;
                ESP_LOGW(TAG, "Obstruction: %s (confirmed on 2nd consecutive reading)",
                         gdo_obstruction_state_to_string(status->obstruction));
                notify_homekit_obstruction(status->obstruction);

                // Arm the staleness watchdog - if gdolib goes quiet on
                // obstruction traffic (it doesn't heartbeat this while
                // idle) before a real Clear ever arrives, this forces one
                // rather than leaving HomeKit stuck on Obstructed forever.
                esp_timer_stop(obstruction_stale_timer);
                esp_timer_start_once(obstruction_stale_timer,
                                      (uint64_t)GDO_OBSTRUCTION_STALE_TIMEOUT_MS * 1000);
            } else {
                // First reading of a new non-clear state - hold it, don't
                // notify HomeKit yet.
                s_pending_obstruction = status->obstruction;
                ESP_LOGW(TAG, "Obstruction: %s reading received (unconfirmed - awaiting 2nd match)",
                         gdo_obstruction_state_to_string(status->obstruction));
            }
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
                set_last_door_state(motor_inferred);
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
                s_transition_start_raw = (uint32_t)status->door_position > 10000
                                             ? 10000 : (uint32_t)status->door_position;
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
//  Obstruction staleness watchdog
// ────────────────────────────────────────────────
//
// See GDO_OBSTRUCTION_STALE_TIMEOUT_MS for why this exists: a confirmed
// Obstructed reading has no guaranteed follow-up Clear event to look
// forward to, since gdolib only reports obstruction status around
// motion/activity, not as a steady idle heartbeat.
static void obstruction_stale_timeout_cb(void *arg)
{
    if (last_obstruction != GDO_OBSTRUCTION_STATE_CLEAR) {
        ESP_LOGW(TAG, "Obstruction: auto-clearing after %d ms with no fresh reading "
                 "to confirm it's still Obstructed (was stuck, not a real sensor Clear)",
                 GDO_OBSTRUCTION_STALE_TIMEOUT_MS);
        last_obstruction = GDO_OBSTRUCTION_STATE_CLEAR;
        notify_homekit_obstruction(GDO_OBSTRUCTION_STATE_CLEAR);
    }
}

// Call once at boot, before any door activity - see GDO_OBSTRUCTION_STALE_TIMEOUT_MS.
static void obstruction_watchdog_init(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = obstruction_stale_timeout_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "obst_stale_wd",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&timer_args, &obstruction_stale_timer);
}


// ────────────────────────────────────────────────
//  Monitoring / recovery watchdog
// ────────────────────────────────────────────────
//
// Runs independently of the GDO callback, so it keeps working even if the
// serial link has gone completely silent.

// Fires once the auto-close warning finishes (see pre_close_warning.h's
// async variant) - runs on its own task, not the watchdog task, so the
// watchdog keeps polling normally through the whole warning duration
// instead of blocking on it.
static void auto_close_warning_complete_cb(void)
{
    ESP_LOGW(TAG, "Auto-close: warning complete, closing now");
    notify_homekit_target_door_state_change(TGT_CLOSED);
    gdo_door_close();
}

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
                uint32_t raw_now = (uint32_t)status.door_position;
                if (raw_now > 10000) raw_now = 10000;

                int32_t moved = (int32_t)raw_now - (int32_t)s_transition_start_raw;
                if (moved < 0) moved = -moved;
                bool stalled = (elapsed >= GDO_DOOR_STALL_CHECK_MS) &&
                               ((uint32_t)moved < GDO_DOOR_STALL_MOVEMENT_MIN);

                // Use the door's own measured travel time once the library
                // has learned it; otherwise fall back to a conservative
                // fixed ceiling. Skip both entirely if the door has plainly
                // never moved at all since the motor turned on - that's not
                // a slow transit, it's a refused/aborted move, and doesn't
                // deserve the same generous ceiling.
                uint16_t measured_ms = (s_transition_target == GDO_DOOR_STATE_OPENING)
                                           ? status.open_ms
                                           : status.close_ms;
                uint32_t timeout_ms = stalled
                                           ? GDO_DOOR_STALL_CHECK_MS
                                           : (measured_ms > 0
                                                  ? (uint32_t)measured_ms + GDO_DOOR_TRANSIT_MARGIN_MS
                                                  : GDO_DOOR_TRANSIT_FALLBACK_MS);

                if (elapsed > timeout_ms) {
                    uint32_t raw = raw_now;

                    gdo_door_state_t resolved = GDO_DOOR_STATE_MAX;

                    // status.door is authoritative whenever it's already
                    // definitive (OPEN/CLOSED/STOPPED). Otherwise - door
                    // still reads OPENING/CLOSING/MAX - fall back to raw
                    // position at an extreme as the best information
                    // available once the timeout above has already elapsed.
                    // NOTE: raw was confirmed in the field to reach an
                    // extreme value well before the door's real, learned
                    // travel time (2.5s vs ~15s in one capture), so this is
                    // a deliberate accuracy/speed tradeoff, not a
                    // guaranteed-correct signal - see chat history around
                    // GDO_DOOR_POS_CONFIRM_GRACE_MS if that tradeoff ever
                    // needs revisiting.
                    if (status.door == GDO_DOOR_STATE_OPEN ||
                        status.door == GDO_DOOR_STATE_CLOSED ||
                        status.door == GDO_DOOR_STATE_STOPPED) {
                        resolved = status.door;
                    } else if (raw <= GDO_DOOR_POS_OPEN_THRESHOLD) {
                        resolved = GDO_DOOR_STATE_OPEN;
                    } else if (raw >= GDO_DOOR_POS_CLOSED_THRESHOLD) {
                        resolved = GDO_DOOR_STATE_CLOSED;
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
                            set_last_door_state(resolved);
                            notify_homekit_current_door_state_change(resolved);
                        }

                        // Also fix HomeKit's target right here rather than
                        // waiting for the separate never-engaged watchdog to
                        // catch it on its own tail - if the motor did fire
                        // (this branch only runs when s_door_in_transition
                        // was true) but the door never actually reached the
                        // commanded target, the target is stale too.
                        gdo_door_state_t requested_target =
                            (status.door_target == 0)     ? GDO_DOOR_STATE_OPEN :
                            (status.door_target == 10000) ? GDO_DOOR_STATE_CLOSED :
                                                             GDO_DOOR_STATE_MAX;
                        if (requested_target != GDO_DOOR_STATE_MAX && requested_target != resolved) {
                            if (resolved == GDO_DOOR_STATE_OPEN) {
                                notify_homekit_target_door_state_change(TGT_OPEN);
                            } else if (resolved == GDO_DOOR_STATE_CLOSED) {
                                notify_homekit_target_door_state_change(TGT_CLOSED);
                            }
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
                            set_last_door_state(GDO_DOOR_STATE_STOPPED);
                            notify_homekit_current_door_state_change(GDO_DOOR_STATE_STOPPED);
                        }
                        gdo_sync();
                    }
                }
            } else {
                ESP_LOGW(TAG, "gdo_get_status() failed (%d) during transition watchdog check", err);
            }
        }

        // 1b) Requested but motor never engaged at all: distinct from #1
        //     above - #1 only arms once motion is confirmed
        //     (s_door_in_transition == true). If the opener refuses to even
        //     start (e.g. beam broken at the exact moment the command
        //     arrived), s_door_in_transition never becomes true and #1 never
        //     fires, leaving HomeKit's target permanently mismatched with
        //     no correction. This check runs independently of #1's gate.
        {
            gdo_status_t status;
            if (gdo_get_status(&status) == ESP_OK) {
                gdo_door_state_t requested_target =
                    (status.door_target == 0)     ? GDO_DOOR_STATE_OPEN :
                    (status.door_target == 10000) ? GDO_DOOR_STATE_CLOSED :
                                                     GDO_DOOR_STATE_MAX;

                bool target_unreached = (requested_target != GDO_DOOR_STATE_MAX) &&
                                         (requested_target != status.door);

                if (!s_door_in_transition && target_unreached) {
                    if (s_pending_target_state != requested_target) {
                        // First time we've observed this particular
                        // unreached target - start its own clock.
                        s_pending_target_state    = requested_target;
                        s_pending_target_start_ms = now;
                    } else if ((uint32_t)(now - s_pending_target_start_ms) > GDO_MOTOR_ENGAGE_TIMEOUT_MS) {
                        ESP_LOGW(TAG,
                                 "%s requested but motor never engaged after %" PRIu32
                                 " ms (no motion ever confirmed) - reverting HomeKit "
                                 "target to match actual state %s",
                                 gdo_door_state_to_string(requested_target),
                                 (uint32_t)(now - s_pending_target_start_ms),
                                 gdo_door_state_to_string(status.door));

                        if (status.door == GDO_DOOR_STATE_OPEN) {
                            notify_homekit_target_door_state_change(TGT_OPEN);
                        } else if (status.door == GDO_DOOR_STATE_CLOSED) {
                            notify_homekit_target_door_state_change(TGT_CLOSED);
                        }
                        if (status.door != GDO_DOOR_STATE_MAX && status.door != last_door) {
                            set_last_door_state(status.door);
                            notify_homekit_current_door_state_change(status.door);
                        }

                        s_pending_target_state = GDO_DOOR_STATE_MAX; // reset until the next request
                    }
                } else {
                    // Either genuinely in transition now, or target matches
                    // reality - nothing pending to watch.
                    s_pending_target_state = GDO_DOOR_STATE_MAX;
                }
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

        // 3) Auto-close: door has been continuously OPEN (confirmed, not
        //    mid-transition - s_door_open_since_ms is only ever non-zero
        //    while last_door == GDO_DOOR_STATE_OPEN, see
        //    set_last_door_state()) for s_auto_close_timeout_ms (runtime-
        //    configurable, see gdo_set_auto_close_timeout_ms()). Skipped
        //    if the last known obstruction reading isn't Clear - err
        //    toward not closing on ambiguous/stale obstruction info rather
        //    than toward closing. Disabled entirely unless explicitly
        //    turned on (see gdo_set_auto_close_enabled(), off by default).
        uint32_t auto_close_timeout_ms = s_auto_close_timeout_ms;
        if (s_auto_close_enabled &&
            s_door_open_since_ms != 0 && !s_auto_close_triggered &&
            (now - s_door_open_since_ms) > auto_close_timeout_ms) {

            if (last_obstruction == GDO_OBSTRUCTION_STATE_CLEAR) {
                ESP_LOGW(TAG,
                         "Auto-close: door open for %" PRId64 " ms (limit %" PRIu32 " ms) - "
                         "sounding warning and closing",
                         now - s_door_open_since_ms, auto_close_timeout_ms);
                s_auto_close_triggered = true;
                pre_close_warning_run_async(PRE_CLOSE_WARNING_DURATION_MS,
                                             auto_close_warning_complete_cb);
            } else {
                ESP_LOGW(TAG,
                         "Auto-close: door open for %" PRId64 " ms (limit %" PRIu32 " ms) but "
                         "last obstruction reading was %s, not Clear - holding off",
                         now - s_door_open_since_ms, auto_close_timeout_ms,
                         gdo_obstruction_state_to_string(last_obstruction));
                // Don't set s_auto_close_triggered - re-check next tick
                // rather than giving up on this open-duration cycle
                // entirely, in case obstruction clears shortly after.
            }
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

    auto_close_settings_load();

    obstruction_watchdog_init();

    gdo_config_t gdo_conf;
    gdo_conf.invert_uart    = true;
    gdo_conf.obst_from_status = true;
    gdo_conf.uart_num       = UART_NUM_1;
    gdo_conf.uart_tx_pin    = GPIO_NUM_1;
    gdo_conf.uart_rx_pin    = GPIO_NUM_2;
    gdo_conf.obst_in_pin    = GPIO_NUM_5;

    s_gdo_synced_sem = xSemaphoreCreateBinary();

    // Sets up the onboard buzzer (GPIO4) and LED (GPIO3) used by the
    // UL-325 pre-close warning in homekit.cpp's gdo_svc_set(). Independent
    // of gdo_init()/gdo_start() below - no shared pins (those own GPIO1/2/5)
    // - so ordering relative to them doesn't matter, but it must run before
    // any door-close request could possibly arrive, hence here at boot.
    pre_close_warning_init();

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