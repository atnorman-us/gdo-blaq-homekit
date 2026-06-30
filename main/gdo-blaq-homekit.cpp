#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "wifi.h"

#include "gdo.h"

#include "tasks.h"
#include "homekit_decl.h"
#include "homekit.h"

static const char* TAG = "test_main";

// Last-known states for filtering duplicate notifications
static gdo_light_state_t       last_light       = GDO_LIGHT_STATE_MAX;
static gdo_lock_state_t        last_lock        = GDO_LOCK_STATE_MAX;
static gdo_door_state_t        last_door        = GDO_DOOR_STATE_MAX;
static gdo_obstruction_state_t last_obstruction = GDO_OBSTRUCTION_STATE_MAX;
static gdo_motion_state_t      last_motion      = GDO_MOTION_STATE_MAX;
static gdo_learn_state_t       last_learn       = GDO_LEARN_STATE_MAX;

static void gdo_event_handler(const gdo_status_t* status, gdo_cb_event_t event, void *arg)
{
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
                ESP_LOGI(TAG, "Rolling code set to %" PRIu32 ", retryng sync",
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

        ESP_LOGI(TAG, "Door raw: %s, %.2f%%, target: %.2f%%",
                gdo_door_state_to_string(status->door),
                (float)status->door_position,
                (float)status->door_target);

        gdo_door_state_t inferred;

        // Fully closed
        if (status->door_position >= 9999) {
            inferred = GDO_DOOR_STATE_CLOSED;
        }
        // Fully open
        else if (status->door_position <= 1) {
            inferred = GDO_DOOR_STATE_OPEN;
        }
        else {
            // Movement direction from position delta
            static float last_pos = -1;

            if (last_pos >= 0) {
                if (status->door_position < last_pos) {
                    inferred = GDO_DOOR_STATE_OPENING;
                } else if (status->door_position > last_pos) {
                    inferred = GDO_DOOR_STATE_CLOSING;
                } else {
                    inferred = last_door; // no movement
                }
            } else {
                inferred = last_door; // first sample
            }

            last_pos = status->door_position;
        }

        // Notify HomeKit only when inferred state changes
        if (inferred != last_door) {
            last_door = inferred;
            notify_homekit_current_door_state_change(inferred);
        }

        // Light updates
        if (status->light != last_light) {
            last_light = status->light;
            notify_homekit_light(status->light);
        }

        break;
    }



    case GDO_CB_EVENT_LEARN:
        if (status->learn != last_learn) {
            last_learn = status->learn;
            ESP_LOGI(TAG, "Learn: %s", gdo_learn_state_to_string(status->learn));
            // Learn → HomeKit is handled in homekit.cpp when supported
        }
        break;

    case GDO_CB_EVENT_OBSTRUCTION:
        if (status->obstruction != last_obstruction) {
            last_obstruction = status->obstruction;
            ESP_LOGI(TAG, "Obstruction: %s",
                     gdo_obstruction_state_to_string(status->obstruction));
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

    default:
        ESP_LOGI(TAG, "Unknown event: %d", event);
        break;
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

    ESP_LOGI(TAG, "GDO started!");
}
