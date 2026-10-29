"""Fault-injected execution of the firmware settings setters."""
import unittest
from test_regressions import function, run_c, ROOT

PRELUDE = r'''
#include <cstdint>
#include <cstring>
#include <cassert>
#include "''' + str(ROOT / 'main/close_schedule.h') + r'''"
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define NVS_READWRITE 1
#define GDO_SETTINGS_NVS_NAMESPACE "settings"
#define GDO_SETTINGS_NVS_KEY_WARN_BUZZ "buzzer"
#define GDO_SETTINGS_NVS_KEY_AUTOCLOSE "timeout"
#define GDO_SETTINGS_NVS_KEY_AC_ENABLED "enabled"
#define GDO_SETTINGS_NVS_KEY_SCHED_BLOB "schedule"
#define ESP_LOGE(...) ((void)0)
using esp_err_t=int;
using nvs_handle_t=int;
static int depth, fail_stage, commit_calls, set_calls;
struct ControlGuard {ControlGuard(){++depth;} ~ControlGuard(){--depth;}};
static void activity_record(const char*,const char*) {}
static uint32_t s_close_generation=7, s_auto_close_timeout_ms=60000;
static bool s_auto_close_enabled=false, buzzer=true;
static gdo_close_schedule_t s_close_schedule={false,120,"UTC0"};
static int32_t s_close_schedule_last_day=123;
static char timezone[96]="UTC0";
struct PersistedSchedule {uint32_t version; gdo_close_schedule_t schedule; int32_t last_day;};
static PersistedSchedule stored;
static int nvs_open(const char*,int,int*h){assert(depth);*h=1;return fail_stage==1 ? ESP_FAIL : ESP_OK;}
static int nvs_set_u8(int,const char*,uint8_t){++set_calls;return fail_stage==2 ? ESP_FAIL : ESP_OK;}
static int nvs_set_u32(int,const char*,uint32_t){++set_calls;return fail_stage==2 ? ESP_FAIL : ESP_OK;}
static int nvs_set_blob(int,const char*,const void*p,size_t n){assert(n==sizeof(stored));++set_calls;if(fail_stage==2)return ESP_FAIL;memcpy(&stored,p,n);return ESP_OK;}
static int nvs_commit(int){++commit_calls;return fail_stage==3 ? ESP_FAIL : ESP_OK;}
static void nvs_close(int){}
static void pre_close_warning_set_buzzer_enabled(bool b){buzzer=b;}
static int clock_sync_apply_timezone(const char*t){assert(depth);if(fail_stage==4)return ESP_FAIL;strcpy(timezone,t);return ESP_OK;}
'''


def setters():
    path='main/gdo-blaq-homekit.cpp'
    return PRELUDE + '\n'.join(function(path, signature) for signature in [
        'static esp_err_t close_schedule_save(',
        'extern "C" esp_err_t gdo_set_pre_close_buzzer(',
        'extern "C" esp_err_t gdo_set_auto_close_timeout_ms(',
        'extern "C" esp_err_t gdo_set_auto_close_enabled(',
        'extern "C" esp_err_t gdo_set_close_schedule(',
        'static esp_err_t close_schedule_last_day_save(',
    ])


class SettingsPersistenceTests(unittest.TestCase):
    def test_scalar_settings_keep_ram_on_each_nvs_failure(self):
        run_c(setters()+r'''
int main(){
 for(int stage=1;stage<=3;++stage){
  fail_stage=stage;commit_calls=0;
  assert(gdo_set_pre_close_buzzer(false)==ESP_FAIL);assert(buzzer);
  assert(gdo_set_auto_close_enabled(true)==ESP_FAIL);assert(!s_auto_close_enabled);
  assert(gdo_set_auto_close_timeout_ms(120000)==ESP_FAIL);assert(s_auto_close_timeout_ms==60000);
  assert(s_close_generation==7);if(stage<3)assert(commit_calls==0);
 }
 fail_stage=0;
 assert(gdo_set_pre_close_buzzer(false)==ESP_OK && !buzzer);
 assert(gdo_set_auto_close_enabled(true)==ESP_OK && s_auto_close_enabled);
 assert(gdo_set_auto_close_timeout_ms(120000)==ESP_OK && s_auto_close_timeout_ms==120000);
 assert(s_close_generation==10);
 assert(gdo_set_auto_close_timeout_ms(1)==ESP_ERR_INVALID_ARG);
}
''',cpp=True)

    def test_schedule_failures_restore_timezone_and_keep_generation_and_day(self):
        run_c(setters()+r'''
int main(){
 gdo_close_schedule_t next={true,130,"EST5EDT,M3.2.0,M11.1.0"};
 for(int stage=1;stage<=4;++stage){
  fail_stage=stage;commit_calls=0;
  assert(gdo_set_close_schedule(&next)==ESP_FAIL);
  assert(!s_close_schedule.enabled && s_close_schedule.minute==120);
  assert(strcmp(timezone,"UTC0")==0 && strcmp(s_close_schedule.tz,"UTC0")==0);
  assert(s_close_generation==7 && s_close_schedule_last_day==123);
  if(stage<3 || stage==4)assert(commit_calls==0);
 }
 fail_stage=0;assert(gdo_set_close_schedule(&next)==ESP_OK);
 assert(s_close_schedule.enabled && s_close_schedule.minute==130);
 assert(strcmp(timezone,next.tz)==0 && s_close_schedule_last_day==0 && s_close_generation==8);
 assert(stored.version==1 && stored.last_day==0 && stored.schedule.minute==130);
 { ControlGuard guard;assert(close_schedule_last_day_save(456)==ESP_OK); }
 assert(s_close_schedule_last_day==456 && stored.last_day==456);
 next.enabled=false;assert(gdo_set_close_schedule(&next)==ESP_OK);assert(s_close_schedule_last_day==456);
 strcpy(next.tz,"UTC0");assert(gdo_set_close_schedule(&next)==ESP_OK);assert(s_close_schedule_last_day==0);
 { ControlGuard guard;fail_stage=2;assert(close_schedule_last_day_save(789)==ESP_FAIL); }
 assert(s_close_schedule_last_day==0);
}
''',cpp=True)

    def test_schedule_load_prefers_atomic_blob_and_reads_legacy_only_when_missing(self):
        source = PRELUDE + r'''
#define NVS_READONLY 0
#define ESP_ERR_NVS_NOT_FOUND -10
#define ESP_LOGI(...) ((void)0)
#define GDO_SETTINGS_NVS_KEY_SCHED_EN "enabled"
#define GDO_SETTINGS_NVS_KEY_SCHED_MIN "minute"
#define GDO_SETTINGS_NVS_KEY_SCHED_TZ "tz"
#define GDO_SETTINGS_NVS_KEY_SCHED_LAST "last"
static int blob_result,legacy_reads;
static int nvs_get_blob(int,const char*,void*p,size_t*n){if(blob_result)return blob_result;memcpy(p,&stored,sizeof(stored));*n=sizeof(stored);return ESP_OK;}
static int nvs_get_u8(int,const char*,uint8_t*p){++legacy_reads;*p=1;return ESP_OK;}
static int nvs_get_u16(int,const char*,uint16_t*p){++legacy_reads;*p=500;return ESP_OK;}
static int nvs_get_i32(int,const char*,int32_t*p){++legacy_reads;*p=999;return ESP_OK;}
static int nvs_get_str(int,const char*,char*p,size_t*){++legacy_reads;strcpy(p,"UTC0");return ESP_OK;}
''' + function('main/gdo-blaq-homekit.cpp', 'static void close_schedule_settings_load(void)')
        run_c(source + r'''
int main(){
 ControlGuard guard;
 stored={1,{true,300,"UTC0"},789};close_schedule_settings_load();
 assert(s_close_schedule.minute==300 && s_close_schedule_last_day==789 && legacy_reads==0);
 blob_result=ESP_ERR_NVS_NOT_FOUND;close_schedule_settings_load();
 assert(s_close_schedule.minute==500 && s_close_schedule_last_day==999 && legacy_reads==4);
 blob_result=0;stored.version=99;legacy_reads=0;close_schedule_settings_load();assert(legacy_reads==0);
 blob_result=ESP_FAIL;close_schedule_settings_load();assert(legacy_reads==0);
}
''',cpp=True)
