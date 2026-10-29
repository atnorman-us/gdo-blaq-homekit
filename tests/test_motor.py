import unittest
from test_regressions import function, run_c
class MotorTests(unittest.TestCase):
    def test_wall_button_infers_from_settled_state(self):
        fn = function('components/gdolib/gdo.c', 'inline static void update_motor_state(gdo_motor_state_t motor_state) {')
        run_c(r'''
#include <stdint.h>
#include <assert.h>
#define ESP_LOGD(...) ((void)0)
#define ESP_OK 0
#define GDO_MOTOR_STATE_ON 1
#define GDO_MOTOR_STATE_OFF 0
#define GDO_DOOR_STATE_OPEN 0
#define GDO_DOOR_STATE_CLOSED 1
#define GDO_DOOR_STATE_OPENING 2
#define GDO_DOOR_STATE_CLOSING 3
#define GDO_EVENT_MOTOR_UPDATE 1
typedef int gdo_motor_state_t;
typedef int gdo_door_state_t;
typedef struct {int event;} gdo_event_t;
static struct {int motor,door,door_target,door_position;uint16_t open_ms,close_ms;} g_status;
static int g_door_start_moving_ms, door_position_sync_timer;
static int esp_timer_get_time(void) {return 1000000;}
static int esp_timer_start_periodic(int t,int d) {return 0;}
static void queue_event(gdo_event_t e) {}
static void update_door_state(gdo_door_state_t s) { g_status.door=s; }
''' + fn + r'''
int main(void) {
 g_status.motor=0; g_status.door=GDO_DOOR_STATE_CLOSED; g_status.door_target=10000;
 g_status.door_position=10000; g_status.open_ms=15000; g_status.close_ms=15000;
 update_motor_state(1); assert(g_status.door==GDO_DOOR_STATE_OPENING);
 g_status.motor=0; g_status.door=GDO_DOOR_STATE_OPEN; g_status.door_target=0; g_status.door_position=0;
 update_motor_state(1); assert(g_status.door==GDO_DOOR_STATE_CLOSING);
}''')
