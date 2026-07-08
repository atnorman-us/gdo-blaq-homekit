// Copyright 2023 Brandon Matthews <thenewwazoo@optimaltour.us>
// All rights reserved. GPLv3 License
static const char *TAG = "HOMEKIT";
#include "homekit_notify.h"
#include <cstring>
#include <atomic>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_mac.h>

#include <hap_apple_servs.h>
#include <hap_apple_chars.h>

#include "homekit_decl.h"
#include <gdo.h>

#include "wifi.h"
#include "diag_webserver.h"

// Defined in gdo-blaq-homekit.cpp - blocks until real GDO sync completes.
extern "C" bool gdo_wait_for_sync(uint32_t timeout_ms);

extern "C" {
    #include "esp_wifi.h"
    #include "esp_wifi_types.h"
    #include "esp_netif.h"
    #include "esp_netif_ip_addr.h"
    #include "esp_event.h"
    #include "esp_event_base.h"

    #include "hap.h"
    #include "hap_platform_os.h"
    #include "hap_platform_keystore.h"

    #include "esp_timer.h"   // ← REQUIRED for esp_timer_get_time()

}

static bool learn_supported = true;
static uint32_t last_hap_restart_ms = 0;
static const uint32_t MIN_RESTART_INTERVAL_MS = 5 * 60 * 1000;  // 5 minutes
static gdo_light_state_t last_light = GDO_LIGHT_STATE_MAX;
static gdo_lock_state_t last_lock = GDO_LOCK_STATE_MAX;
static gdo_door_state_t last_door = GDO_DOOR_STATE_MAX;
static gdo_motion_state_t last_motion = GDO_MOTION_STATE_MAX;
static gdo_obstruction_state_t last_obstruction = GDO_OBSTRUCTION_STATE_MAX;
static gdo_learn_state_t last_learn = GDO_LEARN_STATE_MAX;


#define DEVICE_NAME_SIZE 19
#define SERIAL_NAME_SIZE 18

// Make device_name available
char device_name[DEVICE_NAME_SIZE];

// Make serial_number available
char serial_number[SERIAL_NAME_SIZE];

// Tracks whether at least one controller currently has a live, pair-verified
// HAP session. This is real signal (from the HAP core itself via HAP_EVENT),
// unlike the old timer-based "unhealthy" check - useful for diagnosing
// whether staleness reports correlate with an actual disconnected session.
static std::atomic<int> s_hap_connected_controllers{0};

// Public read-only accessor for diag_webserver.cpp - avoids exposing the
// atomic itself outside this file.
extern "C" int homekit_get_connected_session_count(void)
{
    return s_hap_connected_controllers.load();
}

static void hap_session_event_handler(void* arg, esp_event_base_t event_base,
                                       int32_t event_id, void* event_data)
{
    if (event_base != HAP_EVENT) {
        return;
    }

    const char *ctrl_id = event_data ? (const char *)event_data : "unknown";

    switch (event_id) {
        case HAP_EVENT_CTRL_CONNECTED: {
            int count = s_hap_connected_controllers.fetch_add(1) + 1;
            ESP_LOGI(TAG, "HAP controller connected: %s (active sessions: %d)",
                     ctrl_id, count);
            break;
        }

        case HAP_EVENT_CTRL_DISCONNECTED: {
            int count = s_hap_connected_controllers.fetch_sub(1) - 1;
            if (count < 0) {
                count = 0;
                s_hap_connected_controllers.store(0);
            }
            ESP_LOGW(TAG, "HAP controller disconnected: %s (active sessions: %d)",
                     ctrl_id, count);
            break;
        }

        case HAP_EVENT_CTRL_PAIRED:
            ESP_LOGI(TAG, "HAP controller paired: %s", ctrl_id);
            break;

        case HAP_EVENT_CTRL_UNPAIRED:
            ESP_LOGW(TAG, "HAP controller unpaired: %s", ctrl_id);
            break;

        case HAP_EVENT_PAIRING_STARTED:
            ESP_LOGI(TAG, "HAP pairing started");
            break;

        case HAP_EVENT_PAIRING_ABORTED:
            ESP_LOGW(TAG, "HAP pairing aborted (timeout or wrong setup code)");
            break;

        default:
            // GET_ACC_COMPLETED / GET_CHAR_COMPLETED etc. - too chatty to log
            // at INFO by default, but the case is here if you want them later.
            break;
    }
}

static bool homekit_transport_unhealthy() {
    // NOTE: this used to return true whenever WiFi+IP were healthy and 5
    // minutes had passed since the last restart - i.e. it never actually
    // checked HomeKit transport health at all. In practice that meant
    // restart_homekit_transport() fired every ~5 minutes forever on any
    // stable network, tearing down the live HAP session Home app/Home Hub
    // had open. Symptom: characteristic values were always correct
    // internally, but live push notifications didn't arrive - only a fresh
    // Home app foreground (which forces a reconnect + full read) showed the
    // real state.
    //
    // Disabled until there's a real signal to check here (e.g. something
    // from hap.h indicating the transport actually failed - not available
    // in this file). Restarting a healthy transport on a timer is worse
    // than not restarting at all.
    return false;
}

static void restart_homekit_transport() {
    ESP_LOGW(TAG, "Restarting HomeKit transport...");
    hap_stop();
    vTaskDelay(pdMS_TO_TICKS(250));
    hap_start();
    last_hap_restart_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "HomeKit transport restarted.");
}

// minimal watchdog task to monitor HomeKit transport state and restart if not running
void minimal_hap_watchdog_task(void *pvParameters)
{
    while (true) {
        if (homekit_transport_unhealthy()) {
            ESP_LOGW(TAG, "HomeKit idle with healthy network — restarting transport");
            restart_homekit_transport();
        }

        vTaskDelay(pdMS_TO_TICKS(30000));  // check every 30 seconds
    }
}


// Wifi event handler to handle disconnections and reconnections

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected — retrying");
        esp_wifi_connect();
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Got IP — network restored");
        diag_webserver_restart();
    }
}

// Watchdog task to monitor network connectivity and attempt reconnection if lost
static void network_watchdog_task(void *pvParameters)
{
    while (true) {
        wifi_ap_record_t ap_info;
        esp_netif_ip_info_t ip_info;

        bool wifi_ok = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
        bool ip_ok = (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info) == ESP_OK);

        if (!wifi_ok || !ip_ok) {
            ESP_LOGW(TAG, "Network lost — reconnecting");
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_wifi_connect();
        }

        vTaskDelay(pdMS_TO_TICKS(5000));  // check every 5 seconds
    }
}

// Queue to store GDO notification events
static QueueHandle_t gdo_notif_event_q;

enum class HomeKitNotifDest {
    DoorCurrentState,
    DoorTargetState,
    LockCurrentState,
    LockTargetState,
    Obstruction,
    Light,
    Motion,
    Learn,
};

struct GDOEvent {
    HomeKitNotifDest dest;
    union {
        bool b;
        uint8_t u;
    } value;
};

// Called from app_main() *before* gdo_start(), so the queue exists before
// any GDO event callback could possibly fire. Previously this queue was
// only created inside homekit_task_entry - a separate task that isn't
// guaranteed to have run yet by the time the first GDO events arrive,
// which silently dropped the very first boot-time Light/Lock/Door values
// (logged as "queue full" even though the real cause was "doesn't exist
// yet"). Idempotent - safe to call more than once.
extern "C" void homekit_notif_queue_init(void)
{
    if (!gdo_notif_event_q) {
        gdo_notif_event_q = xQueueCreate(16, sizeof(GDOEvent));
    }
}

static int gdo_svc_set(hap_write_data_t write_data[], int count, void *serv_priv, void *write_priv);
static int light_svc_set(hap_write_data_t write_data[], int count, void *serv_priv, void *write_priv);


/********************************** MAIN LOOP CODE *****************************************/

int identify(hap_acc_t *acc) {
    ESP_LOGI(TAG, "identify called");
    return HAP_SUCCESS;
}

static int learn_svc_set(hap_write_data_t write_data[], int count,
                         void *serv_priv, void *write_priv)
{
    hap_write_data_t *write = &write_data[0];

    if (!strcmp(hap_char_get_type_uuid(write->hc), HAP_CHAR_UUID_ON)) {
        bool enable = write->val.b;
        ESP_LOGI(TAG, "Learn mode: %s", enable ? "Activate" : "Deactivate");

        esp_err_t err = enable ? gdo_activate_learn()
                               : gdo_deactivate_learn();

        if (err == ESP_OK) {
            hap_char_update_val(write->hc, &(write->val));
            *(write->status) = HAP_STATUS_SUCCESS;
        } else {
            ESP_LOGE(TAG, "Learn mode command failed: %d", err);
            *(write->status) = HAP_STATUS_COMM_ERR;
        }

        return HAP_SUCCESS;
    }

    *(write->status) = HAP_STATUS_RES_ABSENT;
    return HAP_FAIL;
}

void homekit_task_entry(void* ctx) {

    uint8_t mac[8] = {0};
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));

    snprintf(device_name, DEVICE_NAME_SIZE, "Garage Door %02X%02X%02X", mac[2], mac[1], mac[0]);
    snprintf(
        serial_number,
        SERIAL_NAME_SIZE,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
    );

    /*
    hap_reset_homekit_data();
    while (1) {}
    */

    hap_acc_t *accessory;
    hap_serv_t *gdo_svc;
    hap_serv_t *motion_svc;
    hap_serv_t *light_svc;

    // Learn Mode must be decided before the accessory is finalized below -
    // HAP's service database can't change after hap_start(). Block briefly
    // for real GDO sync so we know the actual protocol rather than reading
    // gdo_get_status() before sync has had any chance to complete (it needs
    // real UART round-trips with the opener, which haven't happened yet
    // this early in the task). Confirmed via testing: Sec+1.0 openers
    // return ESP_ERR_NOT_SUPPORTED (262) from gdo_activate_learn(), so this
    // is now an evidence-based gate, not an assumption.
    gdo_status_t st;
    bool gdo_synced = gdo_wait_for_sync(8000);
    gdo_get_status(&st);
    learn_supported = gdo_synced && (st.protocol == GDO_PROTOCOL_SEC_PLUS_V2);

    if (!gdo_synced) {
        ESP_LOGW(TAG, "GDO sync did not complete within timeout - hiding Learn Mode tile as a safe default");
    } else if (!learn_supported) {
        ESP_LOGW(TAG, "Learn Mode not supported on protocol %s - tile hidden",
                 gdo_protocol_type_to_string(st.protocol));
    }

    homekit_notif_queue_init(); // already created earlier by app_main() in the normal case; idempotent

    hap_init(HAP_TRANSPORT_WIFI);

    hap_acc_cfg_t config;
    config.name = device_name;
    config.manufacturer = const_cast<char*>("Konnected Inc");
    config.model = const_cast<char*>("blaQ");
    config.serial_num = serial_number;
    config.fw_rev = const_cast<char*>("dev");
    config.hw_rev = NULL;
    config.identify_routine = identify;
    config.cid = HAP_CID_GARAGE_DOOR_OPENER;

    accessory = hap_acc_create(&config);

    // create garage door opener service with optional lock characteristics
    gdo_svc = hap_serv_garage_door_opener_create(
            HOMEKIT_CHARACTERISTIC_CURRENT_DOOR_STATE_OPEN,
            HOMEKIT_CHARACTERISTIC_TARGET_DOOR_STATE_OPEN,
            HOMEKIT_CHARACTERISTIC_OBSTRUCTION_SENSOR_CLEAR);
    hap_serv_add_char(gdo_svc, hap_char_name_create(const_cast<char*>("Konnected blaQ")));

    // The comment above claims "optional lock characteristics" but
    // hap_serv_garage_door_opener_create() only takes door/obstruction
    // params - Lock was never actually attached to the service. Confirmed
    // via real pairing logs: "Events Enabled for aid=1 iid=..." only ever
    // listed 5 characteristics (door x3, motion, light) - no Lock IIDs at
    // all, meaning notify_homekit_current_lock() was updating a
    // characteristic that didn't exist in the published accessory.
    //
    // NOTE: HOMEKIT_CHARACTERISTIC_TARGET_LOCK_STATE_UNSECURED is inferred
    // from this codebase's existing naming convention (matches
    // HOMEKIT_CHARACTERISTIC_CURRENT_LOCK_STATE_UNSECURED, already used
    // elsewhere in this file) - verify it matches your actual SDK headers;
    // if the build fails on this line, grep your hap_apple_*.h headers for
    // the real constant name.
    hap_serv_add_char(gdo_svc, hap_char_lock_current_state_create(
            HOMEKIT_CHARACTERISTIC_CURRENT_LOCK_STATE_UNSECURED));
    hap_serv_add_char(gdo_svc, hap_char_lock_target_state_create(
            HOMEKIT_CHARACTERISTIC_TARGET_LOCK_STATE_UNSECURED));

    hap_serv_set_write_cb(gdo_svc, gdo_svc_set);

    hap_acc_add_serv(accessory, gdo_svc);

    // create the motion sensor service with no optional characteristics (e.g. active)
    motion_svc = hap_serv_motion_sensor_create(false);

    hap_acc_add_serv(accessory, motion_svc);

    // create the light service with no optional characteristics (e.g. brightness)
    light_svc = hap_serv_lightbulb_create(false);

    hap_serv_set_write_cb(light_svc, light_svc_set);

    hap_acc_add_serv(accessory, light_svc);

    hap_add_accessory(accessory);

    // -------------------------------
    // Learn Button HomeKit Switch (Sec+ v2 only)
    // -------------------------------
    hap_serv_t *learn_svc = NULL;

    if (learn_supported) {
        learn_svc = hap_serv_switch_create(false);
        hap_serv_add_char(learn_svc, hap_char_name_create(const_cast<char*>("Learn Mode")));
        hap_serv_set_write_cb(learn_svc, learn_svc_set);
        hap_acc_add_serv(accessory, learn_svc);
        ESP_LOGI(TAG, "Learn Mode supported — HomeKit Learn tile enabled");
    } else {
        ESP_LOGW(TAG, "Learn Mode NOT supported — HomeKit Learn tile hidden");
    }

    // initialize and start homekit
    hap_set_setup_code("251-02-023");  // On Oct 25, 2023, Chamberlain announced they were disabling API
                                       // access for "unauthorized" third parties.
    hap_set_setup_id("KCTD");

    // wifi setup is stuck in the homekit code because homekit sets up some event handlers, and the
    // ordering matters.
    app_wifi_init();

    hap_start();

    // Learn Mode support is now decided early (see gdo_wait_for_sync() call
    // near the top of this function), before hap_add_accessory()/hap_start()
    // finalize the service database. That's the only point it can actually
    // affect whether the tile exists - deciding it here (after hap_start())
    // would be a no-op, which is the mistake this comment used to document.


    // Register WiFi/IP event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // Register HAP session event handler (real signal for controller
    // connect/disconnect/pairing - see hap_session_event_handler above)
    ESP_ERROR_CHECK(esp_event_handler_register(HAP_EVENT, ESP_EVENT_ANY_ID, &hap_session_event_handler, NULL));

    // Start watchdog
    xTaskCreate(network_watchdog_task, "net_watchdog", 4096, NULL, 5, NULL);
    // Start minimal HomeKit watchdog
    xTaskCreate(minimal_hap_watchdog_task, "hap_watchdog", 4096, NULL, 5, NULL);

    GDOEvent e;

    hap_char_t* dest = NULL;

    while (true) {
        hap_val_t value;

        if (xQueueReceive(gdo_notif_event_q, &e, portMAX_DELAY)) {
            switch (e.dest) {
                case HomeKitNotifDest::DoorCurrentState:
                    dest = hap_serv_get_char_by_uuid(gdo_svc, HAP_CHAR_UUID_CURRENT_DOOR_STATE);
                    value.u = e.value.u;
                    break;
                case HomeKitNotifDest::DoorTargetState:
                    dest = hap_serv_get_char_by_uuid(gdo_svc, HAP_CHAR_UUID_TARGET_DOOR_STATE);
                    value.u = e.value.u;
                    break;
                case HomeKitNotifDest::LockCurrentState:
                    dest = hap_serv_get_char_by_uuid(gdo_svc, HAP_CHAR_UUID_LOCK_CURRENT_STATE);
                    value.b = e.value.b;
                    break;
                case HomeKitNotifDest::LockTargetState:
                    dest = hap_serv_get_char_by_uuid(gdo_svc, HAP_CHAR_UUID_LOCK_TARGET_STATE);
                    value.b = e.value.b;
                    break;
                case HomeKitNotifDest::Obstruction:
                    dest = hap_serv_get_char_by_uuid(gdo_svc, HAP_CHAR_UUID_OBSTRUCTION_DETECTED);
                    value.b = e.value.b;
                    break;
                case HomeKitNotifDest::Light:
                    dest = hap_serv_get_char_by_uuid(light_svc, HAP_CHAR_UUID_ON);
                    value.b = e.value.b;
                    break;
                case HomeKitNotifDest::Motion:
                    dest = hap_serv_get_char_by_uuid(motion_svc, HAP_CHAR_UUID_MOTION_DETECTED);
                    value.b = e.value.b;
                    break;
                case HomeKitNotifDest::Learn:
                    if (learn_supported && learn_svc) {
                        dest = hap_serv_get_char_by_uuid(learn_svc, HAP_CHAR_UUID_ON);
                        value.b = e.value.b;
                    } else {
                        dest = NULL;  // ignore learn notifications
                    }
                    break;
            }
            if (dest) {
                ESP_LOGI(TAG, "updating characteristic");
                if (hap_char_update_val(dest, &value) == HAP_FAIL) {
                    ESP_LOGE(TAG, "failed to update characteristic");
                }
            }
        }
    }
}

/******************************** GETTERS AND SETTERS ***************************************/

// this function is called by HomeKit when the value of a characteristic changes (i.e. has been set
// by the user) for the garage door service. It effectuates the value of the characteristic.
static int gdo_svc_set(hap_write_data_t write_data[], int count, void *serv_priv, void *write_priv) {

    int i, ret = HAP_SUCCESS;
    hap_write_data_t *write;

    for (i = 0; i < count; i++) {
        write = &write_data[i];

        if (!strcmp(hap_char_get_type_uuid(write->hc), HAP_CHAR_UUID_TARGET_DOOR_STATE)) {
            ESP_LOGI(TAG, "set door state: %" PRIu32, write->val.u);
            switch (write->val.u) {
                case TGT_OPEN:
                    gdo_door_open();
                    hap_char_update_val(write->hc, &(write->val));
                    *(write->status) = HAP_STATUS_SUCCESS;
                    break;
                case TGT_CLOSED: {

    ESP_LOGI(TAG, "Remote close requested");

    // Retrieve current GDO status so we know the protocol
                    gdo_status_t st;
                    gdo_get_status(&st);

                    bool is_secplus2 = (st.protocol == GDO_PROTOCOL_SEC_PLUS_V2);

                    if (!is_secplus2) {
                        // UL-325 warning for Security+ 1.0
                        ESP_LOGW(TAG, "UL-325 warning: Security+ 1.0 — generating 5-second alert");

                        // Smart panel beeps automatically when the light is flashed
                        for (int i = 0; i < 5; i++) {
                            gdo_light_on();
                            vTaskDelay(pdMS_TO_TICKS(500));

                            gdo_light_off();
                            vTaskDelay(pdMS_TO_TICKS(500));
                        }
                    } else {
                        ESP_LOGI(TAG, "Security+ 2.0 — opener will handle UL-325 warning automatically");
                    }

                    // Now close the door (both protocols)
                    gdo_door_close();

                    hap_char_update_val(write->hc, &(write->val));
                    *(write->status) = HAP_STATUS_SUCCESS;

                    break;
                }

                default:
                    ESP_LOGE(TAG, "invalid target door state set requested: %" PRIu32, write->val.u);
                    *(write->status) = HAP_STATUS_VAL_INVALID;
                    ret = HAP_FAIL;
                    break;
            }
            hap_char_update_val(write->hc, &(write->val));
            *(write->status) = HAP_STATUS_SUCCESS;

        } else if (!strcmp(hap_char_get_type_uuid(write->hc), HAP_CHAR_UUID_LOCK_TARGET_STATE)) {
            ESP_LOGI(TAG, "set lock state: %s", write->val.b ? "Locked" : "Unlocked");
            if (write->val.b) {
                gdo_lock();
            } else {
                gdo_unlock();
            }
            hap_char_update_val(write->hc, &(write->val));
            *(write->status) = HAP_STATUS_SUCCESS;

        } else {
            // no other characteristics are settable
            ESP_LOGE(TAG, "invalid characteristic set, requested UUID: %s", hap_char_get_type_uuid(write->hc));
            *(write->status) = HAP_STATUS_RES_ABSENT;
            ret = HAP_FAIL;

        }
    }

    return ret;
}

GarageDoorCurrentState map_gdo_to_homekit_state(gdo_door_state_t gdo_state) {
    switch (gdo_state) {
        case GDO_DOOR_STATE_OPEN:
            return CURR_OPEN;
        case GDO_DOOR_STATE_CLOSED:
            return CURR_CLOSED;
        case GDO_DOOR_STATE_OPENING:
            return CURR_OPENING;
        case GDO_DOOR_STATE_CLOSING:
            return CURR_CLOSING;
        case GDO_DOOR_STATE_STOPPED:
            return CURR_STOPPED;
        default:
            // unknown or unsupported states; return a default value
            return CURR_STOPPED;
    }
}

void notify_homekit_target_door_state_change(uint8_t tgt) {
    GDOEvent e;
    e.dest = HomeKitNotifDest::DoorTargetState;
    e.value.u = tgt;
    if (!gdo_notif_event_q ||
        xQueueSend(gdo_notif_event_q, &e, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGE(TAG, "could not queue homekit notif of target door state");
    }
}

// this function is called when the current state of the door changes in the world (i.e. we wish to
// update the representation in homekit)
void notify_homekit_current_door_state_change(gdo_door_state_t door) {
    if (door == last_door) return;

    GDOEvent e;
    e.dest = HomeKitNotifDest::DoorCurrentState;
    e.value.u = map_gdo_to_homekit_state(door);

    // Give the consumer loop a brief window to drain (e.g. during the ~250ms
    // HomeKit transport restart in minimal_hap_watchdog_task) instead of
    // dropping instantly. Only commit last_door once the event is actually
    // queued - committing it unconditionally means a dropped notification
    // becomes permanently invisible: the next time the door reaches this
    // same state, the dedup check above would think HomeKit already knows,
    // and never retry.
    if (gdo_notif_event_q &&
        xQueueSend(gdo_notif_event_q, &e, pdMS_TO_TICKS(20)) == pdTRUE) {
        last_door = door;
    } else {
        ESP_LOGE(TAG, "could not queue homekit notif of door current state (queue full)");
    }
}


// this function is called by HomeKit when the value of a characteristic changes (i.e. has been set
// by the user) for the light service. It effectuates the value of the characteristic.
static int light_svc_set(hap_write_data_t write_data[], int count, void *serv_priv, void *write_priv) {
    int i, ret = HAP_SUCCESS;
    hap_write_data_t *write;
    for (i = 0; i < count; i++) {
        write = &write_data[i];

        if (!strcmp(hap_char_get_type_uuid(write->hc), HAP_CHAR_UUID_ON)) {
            ESP_LOGI(TAG, "set light: %s", write->val.b ? "On" : "Off");
            if (write->val.b) {
                gdo_light_on();
            } else {
                gdo_light_off();
            }
            hap_char_update_val(write->hc, &(write->val));
            *(write->status) = HAP_STATUS_SUCCESS;

        } else {
            // no other characteristics are settable
            ESP_LOGE(TAG, "invalid characteristic set, requested UUID: %s", hap_char_get_type_uuid(write->hc));
            *(write->status) = HAP_STATUS_RES_ABSENT;
            ret = HAP_FAIL;

        }
    }

    return ret;
}

// this function is called when the current state of the light changes
void notify_homekit_light(gdo_light_state_t light) {
    if (light == last_light) return;

    GDOEvent e;
    e.dest = HomeKitNotifDest::Light;
    e.value.b = (light == GDO_LIGHT_STATE_ON);

    if (gdo_notif_event_q &&
        xQueueSend(gdo_notif_event_q, &e, pdMS_TO_TICKS(20)) == pdTRUE) {
        last_light = light;
    } else {
        ESP_LOGE(TAG, "could not queue homekit notif of light state (queue full)");
    }
}

// this function is called when the current state of the motion sensor changes in the world
// (i.e. we wish to update the representation in homekit)
void notify_homekit_learn(gdo_learn_state_t learn) {
    if (!learn_supported) return;
    if (learn == last_learn) return;

    GDOEvent e;
    e.dest = HomeKitNotifDest::Learn;
    e.value.b = (learn == GDO_LEARN_STATE_ACTIVE);

    if (gdo_notif_event_q &&
        xQueueSend(gdo_notif_event_q, &e, pdMS_TO_TICKS(20)) == pdTRUE) {
        last_learn = learn;
    } else {
        ESP_LOGE(TAG, "could not queue homekit notif of learn state (queue full)");
    }
}

// this function is called when the current state of the obstruction sensor changes in the world
// (i.e. we wish to update the representation in homekit)
void notify_homekit_obstruction(gdo_obstruction_state_t obstructed) {
    if (obstructed == last_obstruction) return;

    GDOEvent e;
    e.dest = HomeKitNotifDest::Obstruction;
    e.value.b = (obstructed == GDO_OBSTRUCTION_STATE_OBSTRUCTED)
                  ? HOMEKIT_CHARACTERISTIC_OBSTRUCTION_SENSOR_OBSTRUCTED
                  : HOMEKIT_CHARACTERISTIC_OBSTRUCTION_SENSOR_CLEAR;

    if (gdo_notif_event_q &&
        xQueueSend(gdo_notif_event_q, &e, pdMS_TO_TICKS(20)) == pdTRUE) {
        last_obstruction = obstructed;
    } else {
        ESP_LOGE(TAG, "could not queue homekit notif of obstruction state (queue full)");
    }
}

// this function is called when the current state of the lock changes in the world (i.e. we wish to
// update the representation in homekit)
void notify_homekit_current_lock(gdo_lock_state_t lock) {
    if (lock == last_lock) return;

    GDOEvent e;
    e.dest = HomeKitNotifDest::LockCurrentState;
    e.value.b = (lock == GDO_LOCK_STATE_UNLOCKED)
        ? HOMEKIT_CHARACTERISTIC_CURRENT_LOCK_STATE_UNSECURED
        : HOMEKIT_CHARACTERISTIC_CURRENT_LOCK_STATE_SECURED;

    if (gdo_notif_event_q &&
        xQueueSend(gdo_notif_event_q, &e, pdMS_TO_TICKS(20)) == pdTRUE) {
        last_lock = lock;
    } else {
        ESP_LOGE(TAG, "could not queue homekit notif of lock state (queue full)");
    }
}

// this function is called when the state of the motion sensor changes in the world (i.e. we wish to
// update the representation in homekit)
void notify_homekit_motion(gdo_motion_state_t motion) {
    if (motion == last_motion) return;

    GDOEvent e;
    e.dest = HomeKitNotifDest::Motion;
    e.value.b = (motion == GDO_MOTION_STATE_CLEAR)
        ? HOMEKIT_CHARACTERISTIC_MOTION_NOT_DETECTED
        : HOMEKIT_CHARACTERISTIC_MOTION_DETECTED;

    if (gdo_notif_event_q &&
        xQueueSend(gdo_notif_event_q, &e, pdMS_TO_TICKS(20)) == pdTRUE) {
        last_motion = motion;
    } else {
        ESP_LOGE(TAG, "could not queue homekit notif of motion state (queue full)");
    }
}