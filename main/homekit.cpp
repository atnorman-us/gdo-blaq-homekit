// Copyright 2023 Brandon Matthews <thenewwazoo@optimaltour.us>
// All rights reserved. GPLv3 License

#include <cstring>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_mac.h>

#include <hap.h>
#include <hap_apple_servs.h>
#include <hap_apple_chars.h>

#include "homekit_decl.h"
#include <gdo.h>

#include "wifi.h"

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

#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "hap.h"
#include "homekit_decl.h"

static const char *TAG = "HOMEKIT";

hap_char_t *wifi_ssid_char = nullptr;
hap_char_t *wifi_pass_char = nullptr;
static nvs_handle_t wifi_nvs = 0;


static bool learn_supported = false;
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

static bool homekit_transport_unhealthy() {
    // Check WiFi
    wifi_ap_record_t ap_info;
    bool wifi_ok = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);

    // Check IP
    esp_netif_ip_info_t ip_info;
    bool ip_ok = (esp_netif_get_ip_info(
        esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info) == ESP_OK);

    if (!wifi_ok || !ip_ok) {
        return false;  // Never restart HomeKit during network instability
    }

    // Debounce: ensure enough time has passed since last restart
    uint32_t now = esp_timer_get_time() / 1000;  // microseconds → ms
    if (now - last_hap_restart_ms < MIN_RESTART_INTERVAL_MS) {
        return false;
    }

    // If we reach here, network is good and restart interval has passed.
    return true;
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

    if (nvs_open("wifi", NVS_READWRITE, &wifi_nvs) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open Wi-Fi NVS namespace");
    }

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

    gdo_notif_event_q = xQueueCreate(5, sizeof(GDOEvent));

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
    hap_serv_add_char(gdo_svc, hap_char_lock_current_state_create(0));
    hap_serv_add_char(gdo_svc, hap_char_lock_target_state_create(0));

    hap_serv_set_write_cb(gdo_svc, gdo_svc_set);

    hap_acc_add_serv(accessory, gdo_svc);

    // -------------------------------
    // Wi-Fi SSID + Password (runtime update)
    // -------------------------------
    wifi_ssid_char = hap_char_string_create(
        const_cast<char*>("wifi-ssid"),
        HAP_CHAR_PERM_PR | HAP_CHAR_PERM_WR,
        NULL
    );

    wifi_pass_char = hap_char_string_create(
        const_cast<char*>("wifi-pass"),
        HAP_CHAR_PERM_PR | HAP_CHAR_PERM_WR,
        NULL
    );

    // Add to garage door service
    hap_serv_add_char(gdo_svc, wifi_ssid_char);
    hap_serv_add_char(gdo_svc, wifi_pass_char);
   

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

    // Determine if Learn Mode is supported (Sec+ v2 only)
    gdo_status_t st;
    gdo_get_status(&st);

    learn_supported = (st.protocol == GDO_PROTOCOL_SEC_PLUS_V2);

    if (!learn_supported) {
        ESP_LOGW(TAG, "Learn Mode disabled: opener protocol is not Security+ 2.0");
    }


    // Register WiFi/IP event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

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
static int gdo_svc_set(hap_write_data_t write_data[], int count,
                       void *serv_priv, void *write_priv)
{
    int ret = HAP_SUCCESS;

    for (int i = 0; i < count; i++) {
        hap_write_data_t *write = &write_data[i];
        const char *uuid = hap_char_get_type_uuid(write->hc);

        /* ------------------------------
         * Garage Door Target State
         * ------------------------------ */
        if (!strcmp(uuid, HAP_CHAR_UUID_TARGET_DOOR_STATE)) {

            ESP_LOGI(TAG, "set door state: %" PRIu32, write->val.u);

            switch (write->val.u) {
                case TGT_OPEN:
                    gdo_door_open();
                    hap_char_update_val(write->hc, &(write->val));
                    *(write->status) = HAP_STATUS_SUCCESS;
                    break;

                case TGT_CLOSED: {
                    ESP_LOGI(TAG, "Remote close requested");

                    gdo_status_t st;
                    gdo_get_status(&st);

                    bool is_secplus2 = (st.protocol == GDO_PROTOCOL_SEC_PLUS_V2);

                    if (!is_secplus2) {
                        ESP_LOGW(TAG, "UL-325 warning: Security+ 1.0 — generating 5-second alert");

                        for (int i = 0; i < 5; i++) {
                            gdo_light_on();
                            vTaskDelay(pdMS_TO_TICKS(500));
                            gdo_light_off();
                            vTaskDelay(pdMS_TO_TICKS(500));
                        }
                    } else {
                        ESP_LOGI(TAG, "Security+ 2.0 — opener will handle UL-325 warning automatically");
                    }

                    gdo_door_close();

                    hap_char_update_val(write->hc, &(write->val));
                    *(write->status) = HAP_STATUS_SUCCESS;
                    break;
                }

                default:
                    ESP_LOGE(TAG, "invalid target door state: %" PRIu32, write->val.u);
                    *(write->status) = HAP_STATUS_VAL_INVALID;
                    ret = HAP_FAIL;
                    break;
            }
        }

        /* ------------------------------
         * Lock Target State
         * ------------------------------ */
        else if (!strcmp(uuid, HAP_CHAR_UUID_LOCK_TARGET_STATE)) {

            ESP_LOGI(TAG, "set lock state: %s", write->val.b ? "Locked" : "Unlocked");

            if (write->val.b) {
                gdo_lock();
            } else {
                gdo_unlock();
            }

            hap_char_update_val(write->hc, &(write->val));
            *(write->status) = HAP_STATUS_SUCCESS;
        }

        /* ------------------------------
         * Wi-Fi SSID
         * ------------------------------ */
        else if (!strcmp(uuid, "wifi-ssid")) {

            const char *ssid = write->val.s;
            ESP_LOGI(TAG, "Wi-Fi SSID updated: %s", ssid);

            nvs_set_str(wifi_nvs, "wifi_ssid", ssid);
            nvs_commit(wifi_nvs);

            *(write->status) = HAP_STATUS_SUCCESS;
        }

        /* ------------------------------
         * Wi-Fi Password
         * ------------------------------ */
        else if (!strcmp(uuid, "wifi-pass")) {

            const char *pass = write->val.s;
            ESP_LOGI(TAG, "Wi-Fi password updated");

            nvs_set_str(wifi_nvs, "wifi_pass", pass);
            nvs_commit(wifi_nvs);

            *(write->status) = HAP_STATUS_SUCCESS;
        }

        /* ------------------------------
         * Unknown characteristic
         * ------------------------------ */
        else {
            ESP_LOGE(TAG, "invalid characteristic set, requested UUID: %s", uuid);
            *(write->status) = HAP_STATUS_RES_ABSENT;
            ret = HAP_FAIL;
        }
    }

    /* ----------------------------------------------------
     * Apply Wi-Fi changes (if SSID or password was updated)
     * ---------------------------------------------------- */
    wifi_config_t cfg = {};
    size_t ssid_len = sizeof(cfg.sta.ssid);
    size_t pass_len = sizeof(cfg.sta.password);

    nvs_get_str(wifi_nvs, "wifi_ssid", (char*)cfg.sta.ssid, &ssid_len);
    nvs_get_str(wifi_nvs, "wifi_pass", (char*)cfg.sta.password, &pass_len);

    ESP_LOGW(TAG, "Restarting Wi-Fi with new credentials...");
    esp_wifi_stop();
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_start();

    ESP_LOGW(TAG, "Restarting HomeKit transport...");
    hap_stop();
    hap_start();

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

// this function is called when the current state of the door changes in the world (i.e. we wish to
// update the representation in homekit)
void notify_homekit_current_door_state_change(gdo_door_state_t door) {
    if (door == last_door) return;
    last_door = door;

    GDOEvent e;
    e.dest = HomeKitNotifDest::DoorCurrentState;
    e.value.u = map_gdo_to_homekit_state(door);
    if (!gdo_notif_event_q || xQueueSend(gdo_notif_event_q, &e, 0) == errQUEUE_FULL) {
        ESP_LOGE(TAG, "could not queue homekit notif of door current state");
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
    last_light = light;

    GDOEvent e;
    e.dest = HomeKitNotifDest::Light;
    e.value.b = (light == GDO_LIGHT_STATE_ON);
    if (!gdo_notif_event_q || xQueueSend(gdo_notif_event_q, &e, 0) == errQUEUE_FULL) {
        ESP_LOGE(TAG, "could not queue homekit notif of light state");
    }
}

// this function is called when the current state of the motion sensor changes in the world
// (i.e. we wish to update the representation in homekit)
void notify_homekit_learn(gdo_learn_state_t learn) {
    if (!learn_supported) return;
    if (learn == last_learn) return;
    last_learn = learn;

    GDOEvent e;
    e.dest = HomeKitNotifDest::Learn;
    e.value.b = (learn == GDO_LEARN_STATE_ACTIVE);
    xQueueSend(gdo_notif_event_q, &e, 0);
}

// this function is called when the current state of the obstruction sensor changes in the world
// (i.e. we wish to update the representation in homekit)
void notify_homekit_obstruction(gdo_obstruction_state_t obstructed) {
    if (obstructed == last_obstruction) return;
    last_obstruction = obstructed;

    GDOEvent e;
    e.dest = HomeKitNotifDest::Obstruction;
    e.value.b = (obstructed == GDO_OBSTRUCTION_STATE_OBSTRUCTED)
                  ? HOMEKIT_CHARACTERISTIC_OBSTRUCTION_SENSOR_OBSTRUCTED
                  : HOMEKIT_CHARACTERISTIC_OBSTRUCTION_SENSOR_CLEAR;
    xQueueSend(gdo_notif_event_q, &e, 0);
}

// this function is called when the current state of the lock changes in the world (i.e. we wish to
// update the representation in homekit)
void notify_homekit_current_lock(gdo_lock_state_t lock) {
    if (lock == last_lock) return;
    last_lock = lock;

    GDOEvent e;
    e.dest = HomeKitNotifDest::LockCurrentState;
    e.value.b = (lock == GDO_LOCK_STATE_UNLOCKED)
        ? HOMEKIT_CHARACTERISTIC_CURRENT_LOCK_STATE_UNSECURED
        : HOMEKIT_CHARACTERISTIC_CURRENT_LOCK_STATE_SECURED;
    xQueueSend(gdo_notif_event_q, &e, 0);
}

// this function is called when the state of the motion sensor changes in the world (i.e. we wish to
// update the representation in homekit)
void notify_homekit_motion(gdo_motion_state_t motion) {
    if (motion == last_motion) return;
    last_motion = motion;

    GDOEvent e;
    e.dest = HomeKitNotifDest::Motion;
    e.value.b = (motion == GDO_MOTION_STATE_CLEAR)
        ? HOMEKIT_CHARACTERISTIC_MOTION_NOT_DETECTED
        : HOMEKIT_CHARACTERISTIC_MOTION_DETECTED;
    xQueueSend(gdo_notif_event_q, &e, 0);
}

