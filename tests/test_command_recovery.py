import unittest
from test_regressions import ROOT, function, run_c

class CommandRecoveryTests(unittest.TestCase):
    def test_close_returns_before_warning_and_new_command_cancels(self):
        path = 'main/gdo-blaq-homekit.cpp'
        source = (ROOT / path).read_text()
        task = function(path, 'static void user_close_task(') if 'static void user_close_task(' in source else ''
        close = function(path, 'extern "C" esp_err_t gdo_control_close(')
        opening = function(path, 'extern "C" esp_err_t gdo_control_open(')
        run_c(r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <utility>
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_LOGW(...) ((void)0)
#define pdPASS 1
#define tskIDLE_PRIORITY 0
#define PRE_CLOSE_WARNING_DURATION_MS 5000
#define GDO_OBSTRUCTION_STATE_CLEAR 0
#define GDO_DOOR_STATE_OPEN 1
#define GDO_DOOR_STATE_CLOSED 2
#define TGT_OPEN 0
#define TGT_CLOSED 1
using esp_err_t = int;
struct ControlGuard {};
struct gdo_status_t { bool synced; int obstruction; int door; } current={true,0,1};
static uint32_t s_close_generation;
static uint32_t s_user_close_pending_generation;
static int warnings, closes, opens, allocation_failure, task_failure, allocated;
static bool s_user_close_pending;
static std::vector<std::pair<void(*)(void*), void*>> pending_tasks;
static int gdo_get_status(gdo_status_t *p) {*p=current;return ESP_OK;}
static int gdo_door_close() {++closes;return ESP_OK;}
static int gdo_door_open() {++opens;return ESP_OK;}
static void pre_close_warning_run(uint32_t ms) {assert(ms==5000);++warnings;}
static void notify_homekit_target_door_state_change(int) {}
static void *pvPortMalloc(size_t n) {if(allocation_failure)return nullptr;++allocated;return malloc(n);}
static void vPortFree(void *p) {--allocated;free(p);}
static void vTaskDelete(void*) {}
static int xTaskCreate(void (*fn)(void*),const char*,int,void *arg,int,void*) {
 if(task_failure)return 0; pending_tasks.push_back({fn,arg}); return pdPASS;
}
// Runs the OLDEST still-unrun task - the one that would wake up first in
// real FreeRTOS scheduling, since it was spawned first.
static void run_oldest_pending_task() {
 auto task = pending_tasks.front(); pending_tasks.erase(pending_tasks.begin());
 task.first(task.second);
}
''' + task + opening + close + r'''
int main() {
 assert(gdo_control_close()==ESP_OK); assert(warnings==0 && closes==0);
 assert(gdo_control_open()==ESP_OK); run_oldest_pending_task();
 assert(closes==0 && allocated==0);
 assert(gdo_control_close()==ESP_OK); run_oldest_pending_task(); assert(closes==1);
 assert(gdo_control_close()==ESP_OK); current.obstruction=1;run_oldest_pending_task();assert(closes==1);
 current.obstruction=0;
 allocation_failure=1;assert(gdo_control_close()==ESP_ERR_NO_MEM);assert(closes==1);
 allocation_failure=0;task_failure=1;assert(gdo_control_close()==ESP_ERR_NO_MEM);assert(allocated==0);
 task_failure=0;

 // The race this guards against: close, then open cancels it - but the
 // original close's warning task hasn't woken up yet (still "pending" in
 // real FreeRTOS terms). A close attempted right now, in that gap, must
 // not be spuriously rejected just because the stale task hasn't gotten
 // around to clearing s_user_close_pending for itself yet.
 assert(gdo_control_close()==ESP_OK);           // task A armed, generation G1
 assert(gdo_control_open()==ESP_OK);             // cancels A (generation -> G2)
 assert(gdo_control_close()==ESP_OK);            // must succeed immediately, task B armed (generation G3)
 assert(pending_tasks.size()==2);
 int closes_before = closes;
 run_oldest_pending_task();                      // task A wakes: superseded, must not close and must not
                                                   // clear the pending slot out from under task B
 assert(closes==closes_before);
 assert(gdo_control_close()==ESP_ERR_INVALID_STATE); // task B is still genuinely pending - correctly rejected
 run_oldest_pending_task();                      // task B wakes: its generation is current, actually closes
 assert(closes==closes_before+1);
}
''', cpp=True)

    def test_pairing_fault_after_sustained_sync_failure(self):
        path = 'main/gdo-blaq-homekit.cpp'
        source = (ROOT / path).read_text()
        start_label = source.index('case GDO_CB_EVENT_SYNCED:')
        body_start = source.index('\n', start_label) + 1
        body_end = source.index('        break;', body_start)
        block = source[body_start:body_end]
        run_c(r'''
#include <cstdint>
#include <cassert>
#include <cinttypes>
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define TAG "test"
#define GDO_PROTOCOL_SEC_PLUS_V2 1
#define GDO_PAIRING_FAULT_TIMEOUT_MS (15 * 60 * 1000)
struct gdo_status_t { bool synced; int protocol; uint32_t client_id, rolling_code; };
static uint32_t s_sync_retry_count = 0;
static int64_t s_sync_first_failure_ms = 0;
static bool s_gdo_pairing_fault = false;
static void *s_gdo_synced_sem = nullptr;
static int64_t now_val;
static int64_t now_ms() { return now_val; }
static int gdo_set_rolling_code(uint32_t) { return ESP_OK; }
static int gdo_sync() { return ESP_OK; }
static const char *gdo_protocol_type_to_string(int) { return "Security+ 2.0"; }
static void xSemaphoreGive(void*) {}
static void gdo_save_synced_protocol(int) {}
static void gdo_save_rolling_state(uint32_t, uint32_t) {}
static void process_door_position(const gdo_status_t*) {}
static void handle_synced(const gdo_status_t *status) {
''' + block + r'''
}
int main() {
 // A device that has genuinely lost its pairing with the opener (paired-
 // device list cleared, opener firmware update, ...) retries sync forever
 // with no visible difference from ordinary transient noise - this should
 // surface as a distinct fault after sustained failure, without needing
 // to change the retry behavior itself.
 gdo_status_t status = {false, GDO_PROTOCOL_SEC_PLUS_V2, 1, 100};
 now_val = 0;
 handle_synced(&status);
 assert(!s_gdo_pairing_fault);
 now_val = GDO_PAIRING_FAULT_TIMEOUT_MS - 1;
 handle_synced(&status);
 assert(!s_gdo_pairing_fault);
 now_val = GDO_PAIRING_FAULT_TIMEOUT_MS + 1;
 handle_synced(&status);
 assert(s_gdo_pairing_fault);
 // A real sync recovers it and resets the failure streak.
 status.synced = true;
 handle_synced(&status);
 assert(!s_gdo_pairing_fault && s_sync_retry_count == 0 && s_sync_first_failure_ms == 0);
}
''', cpp=True)

    def test_stale_link_requests_status_when_already_synced(self):
        path='main/gdo-blaq-homekit.cpp'
        source=(ROOT/path).read_text()
        start=source.index('        if (s_last_status_event_ms != 0 &&')
        block=source[start:source.index('        // 3) Auto-close:',start)]
        helper=function(path,'static esp_err_t request_gdo_status_recovery(') if 'static esp_err_t request_gdo_status_recovery(' in source else ''
        run_c(r'''
#include <cassert>
#include <cstdint>
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 2
#define ESP_LOGW(...) ((void)0)
#define GDO_LINK_STALE_TIMEOUT_MS 300000
using esp_err_t=int;
struct gdo_status_t {bool synced;};
static bool synced=true;
static int refreshes,syncs;
static int64_t s_last_status_event_ms=1;
static int gdo_get_status(gdo_status_t *s) {s->synced=synced;return ESP_OK;}
static int gdo_sync() {if(synced)return ESP_ERR_INVALID_STATE;++syncs;return ESP_OK;}
static int gdo_refresh_status() {++refreshes;return ESP_OK;}
''' + helper + '\nstatic void tick(int64_t now) {\n' + block + r'''
}
int main() {
 tick(300002); assert(refreshes==1 && syncs==0);
 synced=false;tick(600003);assert(syncs==1);
}
''',cpp=True)
