"""Host tests for the scheduled-close trigger and its completion callback."""
import unittest
from test_regressions import ROOT, function, run_c

PRELUDE = r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include "''' + str(ROOT / 'main/close_schedule.h') + r'''"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define TAG "t"
#define ESP_OK 0
#define ESP_FAIL -1
#define GDO_DOOR_STATE_OPEN 0
#define GDO_DOOR_STATE_CLOSED 1
#define GDO_OBSTRUCTION_STATE_CLEAR 0
#define TGT_CLOSED 1
#define PRE_CLOSE_WARNING_DURATION_MS 5000
struct ControlGuard {};
struct gdo_status_t { bool synced; int door, obstruction; uint32_t door_command_id; } current;
static int last_door, last_obstruction;
static uint32_t s_close_generation;
static bool s_auto_close_triggered, s_user_close_pending;
static gdo_close_schedule_t s_close_schedule;
static int32_t s_close_schedule_last_day;
static uint32_t s_sched_close_generation, s_sched_close_command_id;
static bool s_sched_close_pending;
static int64_t s_door_open_since_ms, s_door_continuously_open_since_ms, fake_now_ms;
static int64_t now_ms() { return fake_now_ms; }
static bool clock_ok; static uint16_t clock_min; static int32_t clock_day;
static int warning_calls, warning_fail, saves, saved_day, refreshes, closes, notifies;
static void (*warning_cb)(void);
static int gdo_get_status(gdo_status_t *s) { *s = current; return ESP_OK; }
static int gdo_door_close() { ++closes; return ESP_OK; }
static void gdo_refresh_status() { ++refreshes; }
static void notify_homekit_target_door_state_change(int) { ++notifies; }
static int gdo_get_close_schedule(gdo_close_schedule_t *o) { *o = s_close_schedule; return ESP_OK; }
static bool clock_sync_local_now(uint16_t *m, int32_t *d) { if(!clock_ok) return false; *m=clock_min; *d=clock_day; return true; }
static int pre_close_warning_run_async(uint32_t, void (*cb)(void)) { ++warning_calls; warning_cb = cb; return warning_fail ? ESP_FAIL : ESP_OK; }
static void close_schedule_last_day_save(int32_t d) { ++saves; saved_day = d; }
'''

RESET = r'''
static void reset() {
 current = {true, GDO_DOOR_STATE_OPEN, GDO_OBSTRUCTION_STATE_CLEAR, 7};
 last_door = GDO_DOOR_STATE_OPEN; last_obstruction = GDO_OBSTRUCTION_STATE_CLEAR;
 s_close_generation = 3; s_auto_close_triggered = false; s_user_close_pending = false;
 s_close_schedule = {true, 23*60, "UTC0"}; s_close_schedule_last_day = 0;
 s_sched_close_pending = false; s_sched_close_generation = s_sched_close_command_id = 0;
 fake_now_ms = 100000000; s_door_open_since_ms = 1000000; s_door_continuously_open_since_ms = 1000000;
 clock_ok = true; clock_min = 23*60 + 1; clock_day = 20000;
 warning_calls = warning_fail = saves = saved_day = refreshes = closes = notifies = 0; warning_cb = nullptr;
}
'''


def sources():
    cb = function('main/gdo-blaq-homekit.cpp', 'static void sched_close_warning_complete_cb(void)')
    chk = function('main/gdo-blaq-homekit.cpp', 'static void sched_close_check(void)')
    return PRELUDE + cb + '\n' + chk + '\n' + RESET


class SchedCloseTests(unittest.TestCase):
    def test_fires_once_inside_window_and_records_day(self):
        run_c(sources() + r'''
int main() {
 reset(); sched_close_check();
 assert(warning_calls==1 && saves==1 && saved_day==20000 && s_close_schedule_last_day==20000);
 assert(s_sched_close_pending && s_sched_close_command_id==7 && s_sched_close_generation==3);
 sched_close_check(); assert(warning_calls==1);          /* still pending */
 s_sched_close_pending = false; sched_close_check();
 assert(warning_calls==1 && saves==1);                   /* already fired this window */
 return 0;
}''', cpp=True)

    def test_does_not_fire_when_conditions_not_met(self):
        run_c(sources() + r'''
int main() {
 reset(); s_close_schedule.enabled=false; sched_close_check(); assert(warning_calls==0);
 reset(); clock_ok=false; sched_close_check(); assert(warning_calls==0);            /* unsynced clock */
 reset(); clock_min=22*60+59; sched_close_check(); assert(warning_calls==0);        /* before window */
 reset(); clock_min=23*60+5; sched_close_check(); assert(warning_calls==0);         /* after window */
 reset(); current.door=GDO_DOOR_STATE_CLOSED; last_door=GDO_DOOR_STATE_CLOSED; s_door_continuously_open_since_ms=0;
 sched_close_check(); assert(warning_calls==0);
 reset(); current.door=GDO_DOOR_STATE_CLOSED; sched_close_check(); assert(warning_calls==0);   /* status closed only */
 reset(); last_door=GDO_DOOR_STATE_CLOSED; sched_close_check(); assert(warning_calls==0);      /* last_door closed only */
 reset(); s_door_continuously_open_since_ms=0; sched_close_check(); assert(warning_calls==0);
 reset(); current.synced=false; sched_close_check(); assert(warning_calls==0);
 reset(); s_auto_close_triggered=true; sched_close_check(); assert(warning_calls==0);
 reset(); s_user_close_pending=true; sched_close_check(); assert(warning_calls==0);
 return 0;
}''', cpp=True)

    def test_door_opened_after_scheduled_minute_is_not_closed(self):
        run_c(sources() + r'''
int main() {
 reset(); clock_min = 23*60+3; s_door_continuously_open_since_ms = fake_now_ms - 30000;   /* opened 30 s ago */
 sched_close_check();
 assert(warning_calls==0 && saves==0 && s_close_schedule_last_day==0 && !s_sched_close_pending);
 return 0;
}''', cpp=True)

    def test_cancelled_auto_close_countdown_reset_does_not_skip_night(self):
        run_c(sources() + r'''
int main() {
 reset(); clock_min = 23*60+3;
 s_door_open_since_ms = fake_now_ms;                                  /* auto-close countdown restarted */
 s_door_continuously_open_since_ms = fake_now_ms - 4*60000;
 sched_close_check(); assert(warning_calls==1 && saves==1);
 return 0;
}''', cpp=True)

    def test_door_open_since_before_window_fires(self):
        run_c(sources() + r'''
int main() {
 reset(); clock_min = 23*60+3; s_door_continuously_open_since_ms = fake_now_ms - 4*60000;   /* open 4 min, window began 3 min ago */
 sched_close_check(); assert(warning_calls==1 && saves==1);
 return 0;
}''', cpp=True)

    def test_obstruction_holds_off_without_burning_the_day(self):
        run_c(sources() + r'''
int main() {
 reset(); current.obstruction=1; sched_close_check();
 assert(warning_calls==0 && refreshes==1 && saves==0 && s_close_schedule_last_day==0);
 current.obstruction=GDO_OBSTRUCTION_STATE_CLEAR; clock_min=23*60+3;   /* clears later in window */
 sched_close_check(); assert(warning_calls==1 && saves==1);
 reset(); last_obstruction=1; sched_close_check();
 assert(warning_calls==0 && refreshes==1);
 return 0;
}''', cpp=True)

    def test_warning_start_failure_retries_next_tick(self):
        run_c(sources() + r'''
int main() {
 reset(); warning_fail=1; sched_close_check();
 assert(warning_calls==1 && saves==0 && !s_sched_close_pending && s_close_schedule_last_day==0);
 warning_fail=0; sched_close_check(); assert(warning_calls==2 && saves==1);
 return 0;
}''', cpp=True)

    def test_midnight_straddling_window_fires_once(self):
        run_c(sources() + r'''
int main() {
 reset(); s_close_schedule.minute = 23*60+58; clock_min = 23*60+59; clock_day = 20000;
 sched_close_check(); assert(warning_calls==1 && saved_day==20000);
 s_sched_close_pending=false; clock_min = 1; clock_day = 20001;       /* 00:01 next day, same window */
 sched_close_check(); assert(warning_calls==1);
 return 0;
}''', cpp=True)

    def test_callback_closes_only_if_still_valid(self):
        run_c(sources() + r'''
static void arm() { reset(); s_sched_close_pending=true; s_sched_close_generation=3; s_sched_close_command_id=7; }
int main() {
 arm(); sched_close_warning_complete_cb();
 assert(closes==1 && notifies==1 && !s_sched_close_pending);
 arm(); s_close_schedule.enabled=false; sched_close_warning_complete_cb(); assert(closes==0 && !s_sched_close_pending);
 arm(); ++s_close_generation; sched_close_warning_complete_cb(); assert(closes==0 && !s_sched_close_pending); /* user/state change */
 arm(); ++current.door_command_id; sched_close_warning_complete_cb(); assert(closes==0);
 arm(); current.obstruction=1; sched_close_warning_complete_cb(); assert(closes==0);
 arm(); last_obstruction=1; sched_close_warning_complete_cb(); assert(closes==0);
 arm(); current.door=GDO_DOOR_STATE_CLOSED; sched_close_warning_complete_cb(); assert(closes==0);
 arm(); current.synced=false; sched_close_warning_complete_cb(); assert(closes==0);
 return 0;
}''', cpp=True)


if __name__ == '__main__':
    unittest.main()
