"""Characterize the SDK event boundary behind door-state notifications."""
import unittest
from test_regressions import function, run_c

SDK = 'esp-homekit-sdk/components/homekit/esp_hap_core/src/esp_hap_char.c'


class HomeKitNotificationTests(unittest.TestCase):
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
static int command_result, opens, closes;
static const char *hap_char_get_type_uuid(hap_char_t *hc) { return hc->uuid; }
static int gdo_control_open() { ++opens; return command_result; }
static int gdo_control_close() { ++closes; return command_result; }
static void gdo_lock() {}
static void gdo_unlock() {}
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
 door.value.u = 0; command_result = 1;
 assert(gdo_svc_set(&write, 1, nullptr, nullptr) == HAP_FAIL);
 assert(status == HAP_STATUS_COMM_ERR && door.value.u == 0 && closes == 2);
 write.val.u = 5;
 assert(gdo_svc_set(&write, 1, nullptr, nullptr) == HAP_FAIL);
 assert(status == HAP_STATUS_VAL_INVALID && closes == 2 && opens == 0);
}
'''
        run_c(source, cpp=True)
