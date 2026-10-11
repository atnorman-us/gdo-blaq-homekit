"""Host tests for the pure scheduled-close logic and form parsing."""
import unittest
from test_regressions import ROOT, run_c

INC = ('#include <assert.h>\n#include <string.h>\n'
       '#include "' + str(ROOT / 'main/close_schedule.h') + '"\n')


class CloseScheduleTests(unittest.TestCase):
    def test_days_from_civil(self):
        run_c(INC + r'''
int main(void) {
 assert(close_schedule_days_from_civil(1970,1,1)==0);
 assert(close_schedule_days_from_civil(2000,1,1)==10957);
 assert(close_schedule_days_from_civil(2024,1,1)==19723);
 assert(close_schedule_days_from_civil(2024,12,31)==20088);
 assert(close_schedule_days_from_civil(2025,1,1)==20089);
 assert(close_schedule_days_from_civil(2025,1,1)-close_schedule_days_from_civil(2024,12,31)==1);
 return 0;
}''')

    def test_should_fire_window_edges(self):
        run_c(INC + r'''
int main(void) {
 const uint16_t S = 23*60;      /* 23:00 */
 const int32_t D = 20000, NONE = 0;
 /* bool synced, door_open, now_min, today, sched_min, last_fired */
 assert( close_schedule_should_fire(true,true,S,   D,S,NONE));
 assert( close_schedule_should_fire(true,true,S+4, D,S,NONE));
 assert(!close_schedule_should_fire(true,true,S+5, D,S,NONE));
 assert(!close_schedule_should_fire(true,true,S-1, D,S,NONE));
 return 0;
}''')

    def test_should_fire_requires_open_synced_and_not_already_fired(self):
        run_c(INC + r'''
int main(void) {
 const uint16_t S = 23*60;
 const int32_t D = 20000;
 assert(!close_schedule_should_fire(true,false,S+1,D,S,0));   /* door not open */
 assert(!close_schedule_should_fire(false,true,S+1,D,S,0));   /* clock unsynced */
 assert(!close_schedule_should_fire(true,true,S+2,D,S,D));    /* already fired today */
 assert( close_schedule_should_fire(true,true,S+2,D,S,D-1));  /* fired yesterday only */
 assert(!close_schedule_should_fire(true,true,1440,D,S,0));   /* invalid now */
 assert(!close_schedule_should_fire(true,true,S,D,1440,0));   /* invalid schedule */
 return 0;
}''')

    def test_window_across_midnight_fires_once(self):
        run_c(INC + r'''
int main(void) {
 const uint16_t S = 23*60 + 58;   /* 23:58, window ends 00:03 */
 const int32_t D = 20000;
 /* Fires at 23:59 on day D, records window day D. */
 assert(close_schedule_should_fire(true,true,23*60+59,D,S,0));
 assert(close_schedule_window_day(23*60+59,D,S)==D);
 /* At 00:01 on day D+1 the same window is still open: must NOT fire again. */
 assert(close_schedule_window_day(1,D+1,S)==D);
 assert(!close_schedule_should_fire(true,true,1,D+1,S,D));
 /* A fresh window the next night fires. */
 assert(close_schedule_should_fire(true,true,23*60+58,D+1,S,D));
 /* 00:03 is out of the window. */
 assert(!close_schedule_should_fire(true,true,3,D+1,S,0));
 return 0;
}''')

    def test_parse_hhmm(self):
        run_c(INC + r'''
int main(void) {
 uint16_t m = 9999;
 assert(close_schedule_parse_hhmm("23:00",&m) && m==1380);
 assert(close_schedule_parse_hhmm("00:00",&m) && m==0);
 assert(close_schedule_parse_hhmm("23:59",&m) && m==1439);
 assert(!close_schedule_parse_hhmm("24:00",&m));
 assert(!close_schedule_parse_hhmm("12:60",&m));
 assert(!close_schedule_parse_hhmm("1:00",&m));
 assert(!close_schedule_parse_hhmm("12-00",&m));
 assert(!close_schedule_parse_hhmm("ab:cd",&m));
 assert(!close_schedule_parse_hhmm("",&m));
 assert(!close_schedule_parse_hhmm(NULL,&m));
 return 0;
}''')

    def test_tz_valid(self):
        run_c(INC + r'''
int main(void) {
 assert(close_schedule_tz_valid("UTC0"));
 assert(close_schedule_tz_valid("PST8PDT,M3.2.0,M11.1.0"));
 assert(close_schedule_tz_valid("CET-1CEST,M3.5.0,M10.5.0/3"));
 assert(close_schedule_tz_valid("<+0530>-5:30"));
 assert(!close_schedule_tz_valid(""));
 assert(!close_schedule_tz_valid(NULL));
 assert(!close_schedule_tz_valid("1UTC"));          /* must start with letter or < */
 assert(!close_schedule_tz_valid("UTC 0"));         /* no spaces */
 assert(!close_schedule_tz_valid("UTC\"0"));        /* no quotes */
 assert(!close_schedule_tz_valid("UTC\\0"));        /* no backslash */
 char longtz[80]; memset(longtz,'A',79); longtz[79]=0;
 assert(!close_schedule_tz_valid(longtz));
 char ok63[64]; memset(ok63,'A',63); ok63[63]=0;
 assert(close_schedule_tz_valid(ok63));
 return 0;
}''')

    def test_form_get_param_decodes_and_rejects(self):
        run_c(INC + r'''
int main(void) {
 char out[16];
 assert(form_get_param("a=1&tz=PST8PDT%2CM3.2.0&b=2","tz",out,sizeof out) && !strcmp(out,"PST8PDT,M3.2.0"));
 assert(form_get_param("time=23%3A00","time",out,sizeof out) && !strcmp(out,"23:00"));
 assert(form_get_param("x=%3C%2B0530%3E-5%3A30","x",out,sizeof out) && !strcmp(out,"<+0530>-5:30"));
 assert(form_get_param("x=a+b","x",out,sizeof out) && !strcmp(out,"a b"));
 assert(!form_get_param("xtz=1","tz",out,sizeof out));      /* key must match whole name */
 assert(!form_get_param("tz=%00abc","tz",out,sizeof out));  /* encoded NUL rejected */
 assert(!form_get_param("tz=0123456789abcdefXYZ","tz",out,sizeof out)); /* too long for out */
 assert(form_get_param("tz=%zz","tz",out,sizeof out) && !strcmp(out,"%zz")); /* bad escape kept literal */
 assert(!form_get_param("a=1","tz",out,sizeof out));
 return 0;
}''')

    def test_parse_form(self):
        run_c(INC + r'''
int main(void) {
 gdo_close_schedule_t s;
 assert(close_schedule_parse_form("enabled=1&time=23%3A00&tz=PST8PDT%2CM3.2.0%2CM11.1.0",&s)==CLOSE_SCHEDULE_FORM_OK);
 assert(s.enabled && s.minute==1380 && !strcmp(s.tz,"PST8PDT,M3.2.0,M11.1.0"));
 assert(close_schedule_parse_form("time=05%3A30&tz=UTC0",&s)==CLOSE_SCHEDULE_FORM_OK);
 assert(!s.enabled && s.minute==330);
 assert(close_schedule_parse_form("enabled=1&time=25%3A00&tz=UTC0",&s)==CLOSE_SCHEDULE_FORM_BAD_TIME);
 assert(close_schedule_parse_form("enabled=1&tz=UTC0",&s)==CLOSE_SCHEDULE_FORM_BAD_TIME);
 assert(close_schedule_parse_form("enabled=1&time=23%3A00&tz=bad%20tz",&s)==CLOSE_SCHEDULE_FORM_BAD_TZ);
 assert(close_schedule_parse_form("enabled=1&time=23%3A00",&s)==CLOSE_SCHEDULE_FORM_BAD_TZ);
 return 0;
}''')


if __name__ == '__main__':
    unittest.main()
