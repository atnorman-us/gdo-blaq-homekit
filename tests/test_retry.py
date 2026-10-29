import unittest
from test_regressions import ROOT, run_c
class RetryTests(unittest.TestCase):
    def test_one_retry_per_command(self):
        run_c('#include <assert.h>\n#include "'+str(ROOT/'main/retry_budget.h')+'"\n'+r'''
int main(void) {
 retry_budget_t b={0};
 assert(retry_budget_step(&b,1,1,1,0,8000)==RETRY_WAIT);
 assert(retry_budget_step(&b,1,1,1,9000,8000)==RETRY_SEND);
 assert(retry_budget_step(&b,1,1,1,18000,8000)==RETRY_REVERT);
 for(int t=19000;t<100000;t+=1000) assert(retry_budget_step(&b,1,1,1,t,8000)==RETRY_WAIT);
 assert(retry_budget_step(&b,2,1,1,100000,8000)==RETRY_WAIT);
 assert(retry_budget_step(&b,2,1,1,109000,8000)==RETRY_SEND);
 // Actual motion, resolution, or cancellation consumes this command.
 assert(retry_budget_step(&b,2,0,1,110000,8000)==RETRY_WAIT);
 assert(retry_budget_step(&b,2,1,1,130000,8000)==RETRY_WAIT);
 assert(retry_budget_step(&b,0,1,1,140000,8000)==RETRY_WAIT);
}''')

    def test_toggle_only_opener_reverts_without_a_retry(self):
        run_c('#include <assert.h>\n#include "'+str(ROOT/'main/retry_budget.h')+'"\n'+r'''
int main(void) {
 retry_budget_t b={0};
 // Security+ 1.0: the engage timeout goes straight to revert, no TOGGLE resend.
 assert(retry_budget_step(&b,1,1,0,0,8000)==RETRY_WAIT);
 assert(retry_budget_step(&b,1,1,0,9000,8000)==RETRY_REVERT);
 for(int t=10000;t<60000;t+=1000) assert(retry_budget_step(&b,1,1,0,t,8000)==RETRY_WAIT);
 // A new request on the same opener is judged afresh, still without a retry.
 assert(retry_budget_step(&b,2,1,0,60000,8000)==RETRY_WAIT);
 assert(retry_budget_step(&b,2,0,0,61000,8000)==RETRY_WAIT);   // it moved: nothing to do
 assert(retry_budget_step(&b,2,1,0,70000,8000)==RETRY_WAIT);
}''')

    def test_gdolib_refuses_to_retry_a_toggle_only_command(self):
        from test_regressions import function
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_SUPPORTED 0x106
static struct { uint32_t door_command_id; bool synced, toggle_only; int32_t door_target; } g_status;
static int opens, closes;
static esp_err_t door_open_impl(void) { ++opens; return ESP_OK; }
static esp_err_t door_close_impl(void) { ++closes; return ESP_OK; }
''' + function('components/gdolib/gdo.c', 'esp_err_t gdo_retry_door_command(uint32_t command_id) {') + r'''
int main(void) {
 g_status.door_command_id = 3; g_status.synced = true; g_status.door_target = 10000;
 assert(gdo_retry_door_command(3) == ESP_OK && closes == 1);
 assert(gdo_retry_door_command(2) == ESP_ERR_INVALID_STATE);
 g_status.toggle_only = true;   // Security+ 1.0 after sync
 assert(gdo_retry_door_command(3) == ESP_ERR_NOT_SUPPORTED);
 g_status.door_target = 0;
 assert(gdo_retry_door_command(3) == ESP_ERR_NOT_SUPPORTED);
 assert(opens == 0 && closes == 1);
}''')
