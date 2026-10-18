"""Characterize the SDK event boundary behind door-state notifications."""
import unittest
from test_regressions import ROOT, function, run_c

SDK = 'esp-homekit-sdk/components/homekit/esp_hap_core/src/esp_hap_char.c'


class HomeKitNotificationTests(unittest.TestCase):
    def test_learn_mode_depends_on_identified_protocol_not_full_sync(self):
        source = (ROOT / 'main/homekit.cpp').read_text()
        start = source.index('    gdo_status_t st = {};')
        end = source.index('    homekit_notif_queue_init();', start)
        learn_mode_decision = source[start:end]

        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define ESP_OK 0
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
enum {
 GDO_PROTOCOL_UNKNOWN = 0,
 GDO_PROTOCOL_SEC_PLUS_V1 = 1,
 GDO_PROTOCOL_SEC_PLUS_V2 = 2,
 GDO_PROTOCOL_MAX = 4,
};
struct gdo_status_t { int protocol; };
static bool learn_supported;
static bool sync_result;
static bool protocol_identified;
static int reported_protocol;
static bool gdo_wait_for_sync(unsigned int) { return sync_result; }
static bool gdo_wait_for_identified_protocol(unsigned int, gdo_status_t *status) {
 status->protocol = reported_protocol;
 return protocol_identified;
}
static void vTaskDelay(unsigned int) {}
static int gdo_get_status(gdo_status_t *status) {
 status->protocol = reported_protocol;
 return 0;
}
static const char *gdo_protocol_type_to_string(int) { return "test"; }
static void decide_learn_mode(void) {
''' + learn_mode_decision + r'''
}
int main(void) {
 sync_result = false;
 protocol_identified = true;
 reported_protocol = GDO_PROTOCOL_SEC_PLUS_V2;
 decide_learn_mode();
 assert(learn_supported);

 sync_result = true;
 protocol_identified = true;
 reported_protocol = GDO_PROTOCOL_SEC_PLUS_V1;
 decide_learn_mode();
 assert(!learn_supported);

 sync_result = false;
 protocol_identified = false;
 reported_protocol = GDO_PROTOCOL_UNKNOWN;
 decide_learn_mode();
 assert(!learn_supported);

 sync_result = false;
 protocol_identified = false;
 reported_protocol = GDO_PROTOCOL_SEC_PLUS_V2;
 decide_learn_mode();
 assert(!learn_supported);
}
''', cpp=True)

    def test_repeated_readable_door_state_does_not_enqueue_an_event(self):
        # Compile the real SDK comparison and enqueue routines. Only FreeRTOS
        # queue/interrupt primitives are replaced at the host boundary.
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define HAP_SUCCESS 0
#define HAP_FAIL -1
#define pdTRUE 1
#define HAP_CHAR_PERM_SPECIAL_READ 64
#define HAP_INTERNAL_EVENT_TRIGGER_NOTIF 1
#define ESP_MFI_DEBUG_INTR(...) ((void)0)
#define hap_platform_memory_free free
enum { HAP_CHAR_FORMAT_BOOL, HAP_CHAR_FORMAT_INT, HAP_CHAR_FORMAT_UINT8,
 HAP_CHAR_FORMAT_UINT16, HAP_CHAR_FORMAT_UINT32, HAP_CHAR_FORMAT_UINT64,
 HAP_CHAR_FORMAT_FLOAT, HAP_CHAR_FORMAT_STRING, HAP_CHAR_FORMAT_DATA,
 HAP_CHAR_FORMAT_TLV8 };
typedef union { bool b; int i; uint32_t u; float f; char *s;
 struct { void *buf; int buflen; } d; } hap_val_t;
typedef struct { int format, permission, owner_ctrl, constraint_flags;
 bool update_called; hap_val_t val, min, max, step; } __hap_char_t;
typedef __hap_char_t hap_char_t;
static int hap_event_queue = 1;
static int queued, triggers;
static hap_char_t *pending;
static int xPortInIsrContext(void) { return 0; }
static int xQueueSend(int q, hap_char_t **hc, int timeout) {
 assert(q == 1); pending = *hc; ++queued; return pdTRUE;
}
static int xQueueSendFromISR(int q, hap_char_t **hc, void *wake) {
 return xQueueSend(q, hc, 0);
}
static void hap_send_event(int event) {
 assert(event == HAP_INTERNAL_EVENT_TRIGGER_NOTIF); ++triggers;
}
'''
        for signature in ['static int hap_queue_event(',
                          'int hap_char_check_val_constraints(',
                          'int hap_char_update_val(']:
            source += function(SDK, signature)
        source += r'''
int main(void) {
 hap_char_t door = {.format = HAP_CHAR_FORMAT_UINT8,
                    .permission = 5, .val = {.u = 1}};
 hap_val_t open = {.u = 0};
 assert(hap_char_update_val(&door, &open) == HAP_SUCCESS);
 assert(door.val.u == 0 && pending == &door);
 assert(queued == 1 && triggers == 1);
 // A GET owner may be cleared, but repeating this value cannot resend it.
 door.owner_ctrl = 7;
 assert(hap_char_update_val(&door, &open) == HAP_SUCCESS);
 assert(door.owner_ctrl == 0);
 assert(queued == 1 && triggers == 1);
 // A subsequent genuine state transition still queues a notification.
 hap_val_t closed = {.u = 1};
 assert(hap_char_update_val(&door, &closed) == HAP_SUCCESS);
 assert(door.val.u == 1 && queued == 2 && triggers == 2);
}
'''
        run_c(source)

    def test_door_write_reports_close_acceptance_or_failure(self):
        source = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_OK 0
#define HAP_SUCCESS 0
#define HAP_FAIL -1
#define HAP_STATUS_SUCCESS 0
#define HAP_STATUS_COMM_ERR -2
#define HAP_STATUS_VAL_INVALID -3
#define HAP_STATUS_RES_ABSENT -4
#define TGT_OPEN 0
#define TGT_CLOSED 1
#define HAP_CHAR_UUID_TARGET_DOOR_STATE "door"
#define HAP_CHAR_UUID_LOCK_TARGET_STATE "lock"
typedef int esp_err_t;
typedef union { uint32_t u; bool b; } hap_val_t;
struct hap_char_t { const char *uuid; hap_val_t value; };
struct hap_write_data_t { hap_char_t *hc; hap_val_t val; int *status; };
#include <atomic>
#define HOMEKIT_CHARACTERISTIC_TARGET_LOCK_STATE_SECURED 1
static int command_result, opens, closes, locks, unlocks;
static std::atomic<uint8_t> last_target{0xFF};
static const char *hap_char_get_type_uuid(hap_char_t *hc) { return hc->uuid; }
static int gdo_control_open() { ++opens; return command_result; }
static int gdo_control_close() { ++closes; return command_result; }
static int gdo_lock() { ++locks; return command_result; }
static int gdo_unlock() { ++unlocks; return command_result; }
static void hap_char_update_val(hap_char_t *hc, hap_val_t *v) { hc->value = *v; }
'''
        source += function('main/homekit.cpp', 'static int gdo_svc_set(hap_write_data_t write_data[], int count, void *serv_priv, void *write_priv) {')
        source += r'''
int main() {
 hap_char_t door = {"door", {.u = 0}};
 int status = 99;
 hap_write_data_t write = {&door, {.u = 1}, &status};
 assert(gdo_svc_set(&write, 1, nullptr, nullptr) == HAP_SUCCESS);
 assert(status == HAP_STATUS_SUCCESS && door.value.u == 1 && closes == 1);
 // An accepted write is recorded for the notify path's deduplication.
 assert(last_target.load() == 1);
 door.value.u = 0; command_result = 1;
 assert(gdo_svc_set(&write, 1, nullptr, nullptr) == HAP_FAIL);
 assert(status == HAP_STATUS_COMM_ERR && door.value.u == 0 && closes == 2);
 assert(last_target.load() == 1);
 write.val.u = 5;
 assert(gdo_svc_set(&write, 1, nullptr, nullptr) == HAP_FAIL);
 assert(status == HAP_STATUS_VAL_INVALID && closes == 2 && opens == 0);

 // Lock Target State: committed only when gdolib accepted the command.
 command_result = 0;
 hap_char_t lock = {"lock", {.u = 0}};
 hap_write_data_t lock_write = {&lock, {.u = 1}, &status};
 assert(gdo_svc_set(&lock_write, 1, nullptr, nullptr) == HAP_SUCCESS);
 assert(status == HAP_STATUS_SUCCESS && lock.value.u == 1 && locks == 1 && unlocks == 0);
 lock_write.val.u = 0; command_result = 0x103;
 assert(gdo_svc_set(&lock_write, 1, nullptr, nullptr) == HAP_FAIL);
 assert(status == HAP_STATUS_COMM_ERR && lock.value.u == 1 && unlocks == 1);
}
'''
        run_c(source, cpp=True)

    def test_light_write_reports_failure_without_committing(self):
        source = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_OK 0
#define HAP_SUCCESS 0
#define HAP_FAIL -1
#define HAP_STATUS_SUCCESS 0
#define HAP_STATUS_COMM_ERR -2
#define HAP_STATUS_RES_ABSENT -4
#define HAP_CHAR_UUID_ON "on"
typedef int esp_err_t;
typedef union { uint32_t u; bool b; } hap_val_t;
struct hap_char_t { const char *uuid; hap_val_t value; };
struct hap_write_data_t { hap_char_t *hc; hap_val_t val; int *status; };
static int command_result, ons, offs;
static const char *hap_char_get_type_uuid(hap_char_t *hc) { return hc->uuid; }
static int gdo_light_on() { ++ons; return command_result; }
static int gdo_light_off() { ++offs; return command_result; }
static void hap_char_update_val(hap_char_t *hc, hap_val_t *v) { hc->value = *v; }
'''
        source += function('main/homekit.cpp', 'static int light_svc_set(hap_write_data_t write_data[], int count, void *serv_priv, void *write_priv) {')
        source += r'''
int main() {
 hap_char_t light = {"on", {.b = false}};
 int status = 99;
 hap_write_data_t write = {&light, {.b = true}, &status};
 assert(light_svc_set(&write, 1, nullptr, nullptr) == HAP_SUCCESS);
 assert(status == HAP_STATUS_SUCCESS && light.value.b && ons == 1);
 // gdolib refused (queue full / not synced): HomeKit must not be told the
 // light is now off.
 write.val.b = false; command_result = 0x101;
 assert(light_svc_set(&write, 1, nullptr, nullptr) == HAP_FAIL);
 assert(status == HAP_STATUS_COMM_ERR && light.value.b && offs == 1);
 hap_char_t other = {"other", {.b = false}};
 hap_write_data_t bad = {&other, {.b = true}, &status};
 assert(light_svc_set(&bad, 1, nullptr, nullptr) == HAP_FAIL);
 assert(status == HAP_STATUS_RES_ABSENT && ons == 1 && offs == 1);
}
'''
        run_c(source, cpp=True)

    def test_target_pushes_are_deduplicated_and_retried_after_a_drop(self):
        # Compile the real producer functions against a fake FreeRTOS queue
        # that can be made full, so the commit-on-success rule is exercised.
        source = r'''
#include <assert.h>
#include <atomic>
#include <stdint.h>
#include <vector>
#define ESP_LOGE(...) ((void)0)
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
#define HOMEKIT_TARGET_UNKNOWN 0xFF
enum gdo_door_state_t { GDO_DOOR_STATE_UNKNOWN, GDO_DOOR_STATE_OPEN, GDO_DOOR_STATE_CLOSED,
 GDO_DOOR_STATE_STOPPED, GDO_DOOR_STATE_OPENING, GDO_DOOR_STATE_CLOSING, GDO_DOOR_STATE_MAX };
enum class HomeKitNotifDest { DoorCurrentState, DoorTargetState };
struct GDOEvent { HomeKitNotifDest dest; union { bool b; uint8_t u; } value; };
typedef int QueueHandle_t;
static QueueHandle_t gdo_notif_event_q = 1;
static std::vector<GDOEvent> queued;
static bool queue_full;
static int xQueueSend(QueueHandle_t, const GDOEvent *e, int) {
 if (queue_full) return pdFALSE; queued.push_back(*e); return pdTRUE; }
static gdo_door_state_t last_door = GDO_DOOR_STATE_MAX;
static std::atomic<uint8_t> last_target{HOMEKIT_TARGET_UNKNOWN};
static uint8_t map_gdo_to_homekit_state(gdo_door_state_t d) { return (uint8_t)d; }
'''
        source += function('main/homekit.cpp', 'void notify_homekit_target_door_state_change(uint8_t tgt) {')
        source += function('main/homekit.cpp', 'void notify_homekit_current_door_state_change(gdo_door_state_t door) {')
        source += r'''
int main() {
 // Every position event during a transition pushes the same Target.
 for (int i = 0; i < 40; ++i) notify_homekit_target_door_state_change(1);
 assert(queued.size() == 1 && queued[0].dest == HomeKitNotifDest::DoorTargetState && queued[0].value.u == 1);
 notify_homekit_target_door_state_change(0);
 assert(queued.size() == 2);
 // A push dropped on a full queue is not remembered as delivered.
 queue_full = true;
 notify_homekit_target_door_state_change(1);
 notify_homekit_current_door_state_change(GDO_DOOR_STATE_OPEN);
 assert(queued.size() == 2 && last_target.load() == 0 && last_door == GDO_DOOR_STATE_MAX);
 queue_full = false;
 notify_homekit_target_door_state_change(1);
 notify_homekit_current_door_state_change(GDO_DOOR_STATE_OPEN);
 assert(queued.size() == 4 && last_target.load() == 1 && last_door == GDO_DOOR_STATE_OPEN);
 // A Target written directly by HomeKit (gdo_svc_set) updates the cache, so
 // a revert back to the previous value is still pushed.
 last_target.store(0);
 notify_homekit_target_door_state_change(1);
 assert(queued.size() == 5);
 notify_homekit_current_door_state_change(GDO_DOOR_STATE_OPEN);
 assert(queued.size() == 5);
}
'''
        run_c(source, cpp=True)
