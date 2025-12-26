// Copyright 2023 Brandon Matthews <thenewwazoo@optimaltour.us>
// All rights reserved. GPLv3 License
#define TAG ("HOMEKIT")

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

#define DEVICE_NAME_SIZE 19
#define SERIAL_NAME_SIZE 18

// Make device_name available
char device_name[DEVICE_NAME_SIZE];

// Make serial_number available
char serial_number[SERIAL_NAME_SIZE];

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

    // create the motion sensor service with no optional characteristics (e.g. active)
    motion_svc = hap_serv_motion_sensor_create(false);

    hap_acc_add_serv(accessory, motion_svc);

    // create the light service with no optional characteristics (e.g. brightness)
    light_svc = hap_serv_lightbulb_create(false);

    hap_serv_set_write_cb(light_svc, light_svc_set);

    hap_acc_add_serv(accessory, light_svc);

    hap_add_accessory(accessory);

    // initialize and start homekit
    hap_set_setup_code("251-02-023");  // On Oct 25, 2023, Chamberlain announced they were disabling API
                                       // access for "unauthorized" third parties.
    hap_set_setup_id("KCTD");

    // wifi setup is stuck in the homekit code because homekit sets up some event handlers, and the
    // ordering matters.
    app_wifi_init();

    hap_start();

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
                case TGT_CLOSED:
                    gdo_door_close();
                    hap_char_update_val(write->hc, &(write->val));
                    *(write->status) = HAP_STATUS_SUCCESS;
                    break;
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

// this function is called when the current state of the door changes in the world (i.e. we wish to
// update the representation in homekit)
void notify_homekit_current_door_state_change(gdo_door_state_t door) {
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

// this function is called when the current state of the obstruction sensor changes in the world
// (i.e. we wish to update the representation in homekit)
void notify_homekit_obstruction(gdo_obstruction_state_t obstructed) {
    GDOEvent e;
    e.dest = HomeKitNotifDest::Obstruction;
    e.value.b = (obstructed == GDO_OBSTRUCTION_STATE_OBSTRUCTED)
                  ? HOMEKIT_CHARACTERISTIC_OBSTRUCTION_SENSOR_OBSTRUCTED
                  : HOMEKIT_CHARACTERISTIC_OBSTRUCTION_SENSOR_CLEAR;
    if (!gdo_notif_event_q || xQueueSend(gdo_notif_event_q, &e, 0) == errQUEUE_FULL) {
        ESP_LOGE(TAG, "could not queue homekit notif of door obstructed");
    }
}

// this function is called when the current state of the lock changes in the world (i.e. we wish to
// update the representation in homekit)
void notify_homekit_current_lock(gdo_lock_state_t lock) {
    GDOEvent e;
    e.dest = HomeKitNotifDest::LockCurrentState;
    e.value.b = (lock == GDO_LOCK_STATE_UNLOCKED)
        ? HOMEKIT_CHARACTERISTIC_CURRENT_LOCK_STATE_UNSECURED
        : HOMEKIT_CHARACTERISTIC_CURRENT_LOCK_STATE_SECURED;
    if (!gdo_notif_event_q || xQueueSend(gdo_notif_event_q, &e, 0) == errQUEUE_FULL) {
        ESP_LOGE(TAG, "could not queue homekit notif of lock state");
    }
}

// this function is called when the state of the light changes in the world (i.e. we wish to update
// the representation in homekit)
void notify_homekit_light(gdo_light_state_t light) {
    GDOEvent e;
    e.dest = HomeKitNotifDest::Light;
    e.value.b = (light == GDO_LIGHT_STATE_ON);
    if (!gdo_notif_event_q || xQueueSend(gdo_notif_event_q, &e, 0) == errQUEUE_FULL) {
        ESP_LOGE(TAG, "could not queue homekit notif of light state");
    }
}

// this function is called when the state of the motion sensor changes in the world (i.e. we wish to
// update the representation in homekit)
void notify_homekit_motion(gdo_motion_state_t motion) {
    GDOEvent e;
    e.dest = HomeKitNotifDest::Motion;
    e.value.b = (motion == GDO_MOTION_STATE_CLEAR)
        ? HOMEKIT_CHARACTERISTIC_MOTION_NOT_DETECTED
        : HOMEKIT_CHARACTERISTIC_MOTION_DETECTED;
    if (!gdo_notif_event_q || xQueueSend(gdo_notif_event_q, &e, 0) == errQUEUE_FULL) {
        ESP_LOGE(TAG, "could not queue homekit notif of motion");
    }
}
