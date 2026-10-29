"""Bounded device activity, independent of log formatting and flash storage."""
import re
import unittest
from test_regressions import ROOT, run_c

class ActivityHistoryTests(unittest.TestCase):
    def test_recent_events_are_bounded_ordered_and_deduplicated(self):
        path=ROOT/'main/activity_history.cpp'
        self.assertTrue(path.exists(), 'Activity history implementation is missing')
        source=path.read_text()
        source=re.sub(r'^#include "(?:freertos/[^"\n]+|esp_timer.h)"\n', '', source, flags=re.M)
        source=source.replace('#include "activity_history.h"', '#include "'+str(ROOT/'main/activity_history.h')+'"')
        run_c(r'''
#include <cassert>
#include <cstring>
#include <cstdint>
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
static int64_t tick=1000000;
static int64_t esp_timer_get_time() {return tick;}
''' + source + r'''
int main() {
 activity_entry_t entries[32];
 assert(activity_read(entries,32)==0);
 activity_record("HomeKit", "Open requested");
 tick+=1000000;activity_record("HomeKit", "Open requested");
 assert(activity_read(entries,32)==1);
 assert(entries[0].uptime_ms==1000);
 assert(!strcmp(entries[0].message,"Open requested"));
 for(int i=0;i<40;i++) {char msg[32];snprintf(msg,sizeof(msg),"Event %d",i);activity_record("Door",msg);}
 assert(activity_read(entries,32)==32);
 assert(!strcmp(entries[0].message,"Event 8"));
 assert(!strcmp(entries[31].message,"Event 39"));
 assert(entries[31].id>entries[0].id);
 assert(activity_read(entries,2)==2);
 assert(!strcmp(entries[0].message,"Event 38"));
 char big[300];memset(big,'X',299);big[299]=0;
 activity_record(big,big);assert(activity_read(entries,1)==1);
 assert(strlen(entries[0].source)==23 && strlen(entries[0].message)==127);
 assert(activity_read(nullptr,1)==0);
 assert(activity_read(entries,0)==0);
}
''',cpp=True)
