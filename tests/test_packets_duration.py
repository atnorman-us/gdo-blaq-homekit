"""Exercise packet validation and full-travel calibration with production C."""
import re
import unittest
from test_regressions import ROOT, function, run_c


def harness():
    public = (ROOT / 'components/gdolib/include/gdo.h').read_text()
    private = (ROOT / 'components/gdolib/gdo_priv.h').read_text()
    enums = '\n'.join(re.findall(r'typedef enum \{.*?\} \w+;', public + private, re.S))
    return r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGV(...) ((void)0)
#define ESP_OK 0
#define UART_EVENT_MAX 20
''' + enums + r'''
typedef struct {int event;} gdo_event_t;
static struct {int door,door_target,door_position,motor,protocol,last_move_direction,obstruction;uint16_t open_ms,close_ms;uint32_t client_id;} g_status;
static struct {bool obst_from_status;} g_config;
static int events, measurements, updates, status_requests, g_door_start_moving_ms, door_position_sync_timer, gdo_sync_task_handle;
static int64_t now;
static int64_t g_last_rx_ms;
static struct {int64_t last_packet_us,last_door_status_us;} g_rx_health;
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)

static int64_t esp_timer_get_time(void) { return now; }
static int esp_timer_start_periodic(int t,int d) {return 0;}
static void esp_timer_stop(int t) {}
static void xTaskNotifyGive(int t) {}
static void get_status(void) { ++status_requests; }
static void get_openings(void) {}
static void queue_event(gdo_event_t e) {events++; if(e.event==GDO_EVENT_DOOR_OPEN_DURATION_MEASUREMENT || e.event==GDO_EVENT_DOOR_CLOSE_DURATION_MEASUREMENT) measurements++;}
''' + function('components/gdolib/gdo.c', 'static void update_door_state(const gdo_door_state_t door_state) {')


class PacketDurationTests(unittest.TestCase):
    def packet_source(self, extra_stubs=''):
        stubs = '\n'.join('static void %s(int a) {updates++;}' % name for name in (
            'update_light_state', 'update_lock_state', 'update_learn_state',
            'update_obstruction_state', 'handle_light_action', 'update_motor_state',
            'update_button_state', 'update_motion_state', 'update_ttc', 'update_battery_state'))
        return harness() + '\n#include "' + str(ROOT / 'components/gdolib/secplus.c') + '"\n' + stubs + r'''
static void update_openings(int a,int b) {updates++;}
static void update_paired_devices(int a,int b) {updates++;}
''' + extra_stubs + '\nstatic int64_t obstruction_confirm_due_us;\nstatic unsigned obstruction_confirm_attempts;\n' + function('components/gdolib/gdo.c', 'static void obstruction_confirm_poll(') + function('components/gdolib/gdo.c', 'static void obstruction_confirm_request(') + function('components/gdolib/gdo.c', 'static void record_received_packet(') + function('components/gdolib/gdo.c', 'static void log_rejected_packet(') \
    + function('components/gdolib/gdo.c', 'static void decode_packet(uint8_t *packet) {')

    def test_receive_age_tracks_reports_not_interpolation_or_echoes(self):
        run_c(self.packet_source() + r'''
int main(void) {
 uint8_t packet[19];
 g_status.door=GDO_DOOR_STATE_CLOSED;
 now=1000000;
 assert(encode_wireline(123,0x123456,0x181,packet)==0);
 decode_packet(packet);
 assert(g_rx_health.last_packet_us==1000000 && g_rx_health.last_door_status_us==1000000);
 now=2000000; decode_packet(packet); // identical state is still a fresh report
 assert(g_rx_health.last_door_status_us==2000000);
 now=3000000; packet[17]^=1; decode_packet(packet);
 assert(g_rx_health.last_packet_us==2000000 && g_rx_health.last_door_status_us==2000000);
 packet[17]^=1; g_status.client_id=0x123456; decode_packet(packet);
 assert(g_rx_health.last_packet_us==2000000); // local echo is not opener health
 g_status.client_id=0; now=4000000;
 assert(encode_wireline(124,0x123456,0xf81,packet)==0); decode_packet(packet);
 assert(g_rx_health.last_door_status_us==2000000); // invalid state is not confirmation
 now=5000000; update_door_state(GDO_DOOR_STATE_CLOSING);
 assert(g_rx_health.last_door_status_us==2000000); // estimates cannot refresh telemetry
}''')

    def test_v1_freshness_requires_a_valid_door_report(self):
        source = self.packet_source() + '\nstatic unsigned g_v1_rx_queries;\nstatic void queue_v1_command(int) {}\n'
        source += function('components/gdolib/gdo.c', 'static inline bool v1_is_query(')
        source += function('components/gdolib/gdo.c', 'static void decode_v1_packet(')
        run_c(source + r'''
int main(void) {
 uint8_t packet[2] = {V1_CMD_QUERY_DOOR_STATUS, 2};
 now=1000000;decode_v1_packet(packet);
 assert(g_rx_health.last_packet_us==1000000 && g_rx_health.last_door_status_us==1000000);
 now=2000000;decode_v1_packet(packet);
 assert(g_rx_health.last_door_status_us==2000000);
 now=3000000;packet[1]=3;decode_v1_packet(packet);
 assert(g_rx_health.last_door_status_us==2000000);
 now=4000000;packet[0]=V1_CMD_QUERY_OTHER_STATUS;decode_v1_packet(packet);
 assert(g_rx_health.last_packet_us==4000000 && g_rx_health.last_door_status_us==2000000);
 now=5000000;packet[0]=0xff;decode_v1_packet(packet);
 assert(g_rx_health.last_packet_us==4000000);
}''')

    def test_bad_parity_cannot_mutate_status_or_emit_events(self):
        run_c(self.packet_source() + r'''
int main(void) {
 uint8_t packet[19]; uint32_t rolling,data; uint64_t fixed;
 now=1000000; g_status.door=GDO_DOOR_STATE_CLOSED;
 assert(encode_wireline(123,0x123456,0x181,packet)==0);
 packet[17]^=1;
 assert(decode_wireline(packet,&rolling,&fixed,&data)==-1);
 decode_packet(packet);
 assert(g_status.door==GDO_DOOR_STATE_CLOSED);
 assert(events==0 && updates==0);
 packet[17]^=1; decode_packet(packet);
 assert(g_status.door==GDO_DOOR_STATE_OPEN && events==1 && updates==3);
}''')

    def test_out_of_range_status_rejects_whole_packet(self):
        run_c(self.packet_source() + r'''
int main(void) {
 uint8_t packet[19]; g_status.door=GDO_DOOR_STATE_CLOSED;
 assert(encode_wireline(123,0x123456,0xf81,packet)==0);
 decode_packet(packet);
 assert(g_status.door==GDO_DOOR_STATE_CLOSED && events==0 && updates==0);
 update_door_state((gdo_door_state_t)-1);
 update_door_state(GDO_DOOR_STATE_MAX);
 assert(events==0);
}''')

    def test_oversized_reads_resync_on_start_signature(self):
        # Field logs: during motor/light activity a read arrives as 21 bytes
        # (1 leading break byte + 19-byte packet + 1 trailing byte). Dumping
        # (size - 19) bytes from the FRONT also dumped the 0x55 start byte,
        # so the valid packet was rejected as a "signature error" of
        # exactly 0x01 0x00 xx (the shifted header) every time.
        stub = 'static void print_buffer(int p, uint8_t *b, bool tx) {(void)p;(void)b;(void)tx;}\n'
        run_c(self.packet_source(stub) + function('components/gdolib/gdo.c', 'static int decode_rx_stream(') + r'''
static void reset(void) { g_status.door = GDO_DOOR_STATE_CLOSED; events = updates = 0; now = 1000000; }
int main(void) {
 uint8_t pkt[19], buf[64], pkt2[19];
 assert(encode_wireline(123, 0x123456, 0x181, pkt) == 0);   // STATUS: door Open
 assert(encode_wireline(124, 0x123456, 0x181, pkt2) == 0);
 assert(pkt[0] == 0x55 && pkt[1] == 0x01 && pkt[2] == 0x00);

 // Each shape must decode the packet exactly once and update the door.
 struct { int lead, trail; } shapes[] = {{0,0},{1,0},{2,0},{0,1},{1,1},{1,2},{3,3}};
 for (unsigned i = 0; i < sizeof(shapes)/sizeof(shapes[0]); ++i) {
  reset(); memset(buf, 0, sizeof(buf));
  memcpy(buf + shapes[i].lead, pkt, 19);
  size_t len = shapes[i].lead + 19 + shapes[i].trail;
  assert(decode_rx_stream(buf, len, 19) == 1);
  assert(g_status.door == GDO_DOOR_STATE_OPEN);
 }

 // Two packets in one read both decode.
 reset(); memset(buf, 0, sizeof(buf));
 memcpy(buf + 1, pkt, 19); memcpy(buf + 21, pkt2, 19);
 assert(decode_rx_stream(buf, 41, 19) == 2);

 // No start signature anywhere: nothing decoded, state untouched.
 reset(); memset(buf, 0xAA, sizeof(buf));
 assert(decode_rx_stream(buf, 21, 19) == 0);
 assert(g_status.door == GDO_DOOR_STATE_CLOSED && events == 0 && updates == 0);

 // A start signature too close to the end to hold a whole packet is ignored.
 reset(); memset(buf, 0, sizeof(buf));
 memcpy(buf + 5, pkt, 19);
 assert(decode_rx_stream(buf, 20, 19) == 0);
}''')

    def test_first_obstruction_event_after_boot_requests_status_not_toggle(self):
        run_c(self.packet_source() + r'''
int main(void) {
 uint8_t packet[19];
 g_config.obst_from_status = true;
 // At boot, obstruction starts unknown (GDO_OBSTRUCTION_STATE_MAX), not
 // Clear. Confirmed in the field: an OBST_1/OBST_2 pair the opener sends
 // unprompted right at boot (unrelated to any real obstruction - possibly
 // its own power-on self-test of the photo-eye pair) toggled straight
 // from "unknown" to "Obstructed" with the door closed and clear the
 // whole time, since a bare toggle can't distinguish "was clear" from
 // "never established." It should request a real status frame instead.
 g_status.obstruction = GDO_OBSTRUCTION_STATE_MAX;
 now = 2000000;
 assert(encode_wireline(123, 0x123456, 0x084, packet) == 0);
 decode_packet(packet);
 assert(status_requests == 1 && updates == 0);
 assert(g_status.obstruction == GDO_OBSTRUCTION_STATE_MAX);
 // A second event in the pending confirmation cycle shares the request.
 g_status.obstruction = GDO_OBSTRUCTION_STATE_CLEAR;
 now = 4000000; // clear the 1s debounce window
 assert(encode_wireline(124, 0x123456, 0x084, packet) == 0);
 decode_packet(packet);
 assert(updates == 1 && status_requests == 1);
}
''')

    def test_closed_door_obstruction_confirmation_is_bounded(self):
        source = self.packet_source().replace(
            'static void update_obstruction_state(int a) {updates++;}',
            function('components/gdolib/gdo.c', 'inline static void update_obstruction_state('))
        run_c(source + r'''
int main(void) {
 uint8_t packet[19];
 g_config.obst_from_status=true;
 g_status.door=GDO_DOOR_STATE_CLOSED;
 g_status.obstruction=GDO_OBSTRUCTION_STATE_CLEAR;
 now=2000000;
 assert(encode_wireline(123,0x123456,0x84,packet)==0); decode_packet(packet);
 assert(g_status.obstruction==GDO_OBSTRUCTION_STATE_OBSTRUCTED && status_requests==1);
 assert(encode_wireline(123,0x123456,0x85,packet)==0); decode_packet(packet); decode_packet(packet);
 assert(status_requests==1); // duplicate events coalesce
 now=3000000; obstruction_confirm_poll(); assert(status_requests==2);
 now=4000000; obstruction_confirm_poll(); assert(status_requests==3);
 now=5000000; obstruction_confirm_poll();
 assert(status_requests==3 && obstruction_confirm_due_us==0);
 assert(g_status.obstruction==GDO_OBSTRUCTION_STATE_OBSTRUCTED); // timeout is not clear
 assert(encode_wireline(124,0x123456,0x84,packet)==0); decode_packet(packet);
 assert(status_requests==4 && g_status.obstruction==GDO_OBSTRUCTION_STATE_OBSTRUCTED);
 // Captured closed/clear STATUS cancels pending retries and clears the UI.
 assert(encode_wireline(125,0x123456,0x40600281,packet)==0); decode_packet(packet);
 assert(g_status.obstruction==GDO_OBSTRUCTION_STATE_CLEAR && !obstruction_confirm_due_us);
 now=6000000; obstruction_confirm_poll(); assert(status_requests==4);
 // OBST_2 alone also requests confirmation; a blocked response stays blocked.
 assert(encode_wireline(126,0x123456,0x85,packet)==0); decode_packet(packet);
 assert(status_requests==5);
 assert(encode_wireline(127,0x123456,0x40200281,packet)==0); decode_packet(packet);
 assert(g_status.obstruction==GDO_OBSTRUCTION_STATE_OBSTRUCTED && !obstruction_confirm_due_us);
}
''')

    def test_status_reconciles_obstruction_after_toggle(self):
        source = self.packet_source().replace(
            'static void update_obstruction_state(int a) {updates++;}',
            function('components/gdolib/gdo.c', 'inline static void update_obstruction_state('))
        run_c(source + r'''
int main(void) {
 uint8_t packet[19];
 g_config.obst_from_status=true;
 g_status.door=GDO_DOOR_STATE_OPEN;
 g_status.obstruction=GDO_OBSTRUCTION_STATE_CLEAR;
 now=2000000;
 assert(encode_wireline(123,0x123456,0x84,packet)==0);
 decode_packet(packet);
 assert(g_status.obstruction==GDO_OBSTRUCTION_STATE_OBSTRUCTED);
 // From the field log: STATUS 0x46600181 follows OBST_1.
 assert(encode_wireline(124,0x123456,0x46600181,packet)==0);
 int before=events;
 decode_packet(packet);
 assert(g_status.obstruction==GDO_OBSTRUCTION_STATE_CLEAR);
 assert(events==before+1); // The clear must reach the application/HomeKit.
 before=events; decode_packet(packet);
 assert(events==before); // Repeated status does not produce duplicate events.
 // A genuine blocked status must still assert obstruction immediately.
 assert(encode_wireline(125,0x123456,0x46200181,packet)==0);
 decode_packet(packet);
 assert(g_status.obstruction==GDO_OBSTRUCTION_STATE_OBSTRUCTED);
 // Invalid packets cannot clear an obstruction.
 assert(encode_wireline(126,0x123456,0x42600281,packet)==0);
 packet[17]^=1; decode_packet(packet);
 assert(g_status.obstruction==GDO_OBSTRUCTION_STATE_OBSTRUCTED);
 packet[17]^=1; decode_packet(packet);
 assert(g_status.obstruction==GDO_OBSTRUCTION_STATE_CLEAR);
 // Do not override the separate GPIO obstruction source.
 g_config.obst_from_status=false;
 g_status.obstruction=GDO_OBSTRUCTION_STATE_OBSTRUCTED;
 decode_packet(packet);
 assert(g_status.obstruction==GDO_OBSTRUCTION_STATE_OBSTRUCTED);
}''')

    def test_battery_state_string_lookup_never_reads_out_of_bounds(self):
        fn = function('components/gdolib/gdo_utils.c', 'const char* gdo_battery_state_to_string(')
        run_c(r'''
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stddef.h>
typedef enum {
 GDO_BATT_STATE_UNKNOWN = 0,
 GDO_BATT_STATE_CHARGING = 0x6,
 GDO_BATT_STATE_FULL = 0x8,
 GDO_BATT_STATE_MAX = 0xff,
} gdo_battery_state_t;
static const char *gdo_battery_state_str[] = {"Unknown", "Charging", "Full"};
''' + fn + r'''
int main(void) {
 assert(!strcmp(gdo_battery_state_to_string(GDO_BATT_STATE_UNKNOWN), "Unknown"));
 assert(!strcmp(gdo_battery_state_to_string(GDO_BATT_STATE_CHARGING), "Charging"));
 assert(!strcmp(gdo_battery_state_to_string(GDO_BATT_STATE_FULL), "Full"));
 // This enum's real wire values (0, 6, 8, 0xff) are sparse, not sequential
 // indices, so it can never be used to index gdo_battery_state_str[]
 // directly (only 3 entries) - every other raw byte a corrupted or
 // not-yet-understood frame could produce must resolve safely instead of
 // reading past the array. Confirmed under ASan across the full range a
 // raw wire byte can actually take.
 for (int b = 0; b < 256; ++b) {
  if (b == GDO_BATT_STATE_UNKNOWN || b == GDO_BATT_STATE_CHARGING || b == GDO_BATT_STATE_FULL) continue;
  assert(gdo_battery_state_to_string((gdo_battery_state_t)b) != NULL);
 }
}
''')

    def test_interrupted_and_reversed_trips_are_not_calibrated(self):
        for opening in (True, False):
            for interrupted in ('STOPPED', 'CLOSING' if opening else 'OPENING'):
                with self.subTest(opening=opening, interrupted=interrupted):
                    start, moving, end, duration = ('CLOSED','OPENING','OPEN','open_ms') if opening else ('OPEN','CLOSING','CLOSED','close_ms')
                    run_c(harness() + f'''
int main(void) {{
 g_status.door=GDO_DOOR_STATE_{start}; now=1000000;
 update_door_state(GDO_DOOR_STATE_{moving}); now=4000000;
 update_door_state(GDO_DOOR_STATE_{interrupted}); now=5000000;
 update_door_state(GDO_DOOR_STATE_{moving}); now=10000000;
 update_door_state(GDO_DOOR_STATE_{end});
 assert(g_status.{duration}==0 && measurements==0);
 update_door_state(GDO_DOOR_STATE_{start}); now=20000000;
 update_door_state(GDO_DOOR_STATE_{moving}); now=35000000;
 update_door_state(GDO_DOOR_STATE_{end});
 assert(g_status.{duration}==15000 && measurements==1);
}}''')
