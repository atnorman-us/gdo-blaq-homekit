"""Execute production OTA guard and browser recovery functions."""
import unittest
from test_regressions import function, run_c
from test_schedule_ui import run_node

class AdminOtaTests(unittest.TestCase):
    def test_durable_slot_guard_and_write_failures(self):
        source=r'''
#include <cassert>
#include <cstdint>
#include <cstddef>

typedef int nvs_handle_t;
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 2
#define NVS_READWRITE 1
struct esp_partition_t {const char *label;};
static int open_error, set_error, commit_error, read_error=2, commits;
static uint8_t durable;
static int nvs_open(const char*, int, int*h){*h=1;return open_error;}
static int nvs_get_u8(int,const char*,uint8_t*v){*v=durable;return read_error;}
static int nvs_set_u8(int,const char*,uint8_t v){if(!set_error){durable=v;read_error=0;}return set_error;}
static int nvs_commit(int){++commits;return commit_error;}
static void nvs_close(int){}
'''
        source+=function('main/diag_webserver.cpp','static bool ota_slot_eligible(')+'\n'
        source+=function('main/diag_webserver.cpp','static bool ota_block_slot(')+'\n'
        source+=r'''
int main(){
 esp_partition_t slot={"ota_1"};
 assert(!ota_slot_eligible(&slot)); // unknown older firmware may read stale legacy storage
 assert(ota_block_slot(&slot,false)); assert(ota_slot_eligible(&slot)); // healthy compatible boot
 assert(ota_block_slot(&slot,true)); assert(!ota_slot_eligible(&slot));
 // No RAM pending pointer is involved: reopening storage preserves the guard.
 assert(!ota_slot_eligible(&slot));
 assert(ota_block_slot(&slot,false)); assert(ota_slot_eligible(&slot));
 int old=commits;set_error=1;assert(!ota_block_slot(&slot,true));assert(commits==old);
 set_error=0;commit_error=1;assert(!ota_block_slot(&slot,true));
 open_error=1;assert(!ota_slot_eligible(&slot));assert(!ota_block_slot(&slot,false));
 open_error=0;read_error=1;assert(!ota_slot_eligible(&slot));
}
'''
        run_c(source,cpp=True)

    def test_pending_review_restores_and_expires(self):
        result=run_node(r'''
renderOtaReview({ota_pending:true,ota_pending_version:'v2',ota_pending_partition:'ota_1',ota_confirm_remaining_ms:1001});
assert.equal(document.getElementById('fwConfirmRow').style.display,'flex');
assert(document.getElementById('fwStatus').textContent.includes('2 seconds'));
renderOtaReview({ota_pending:false});
assert.equal(document.getElementById('fwConfirmRow').style.display,'none');
assert.equal(document.getElementById('fwUploadRow').style.display,'flex');
assert.equal(ageText(-1),'not yet received');
''')
        self.assertEqual(result.returncode,0,result.stderr)

    def test_password_change_uses_old_password_and_replaces_only_on_success(self):
        result=run_node(r'''
(async()=>{
 global.alert=()=>{};
 let answers=['oldpassword','newpassword','newpassword'];
 askPassword=async()=>answers.shift();
 adminToken='oldpassword';
 global.fetch=async(url,options)=>{assert.equal(url,'/admin/change-password');assert.equal(options.headers['X-GDO-Token'],'oldpassword');assert.equal(options.body,'newpassword');return {ok:true};};
 await changePassword();assert.equal(adminToken,'newpassword');
 answers=['oldpassword','newpassword','newpassword'];adminToken='oldpassword';
 global.fetch=async()=>({ok:false,text:async()=> 'Failed storage'});
 await changePassword();assert.equal(adminToken,'oldpassword');
})().catch(e=>{console.error(e);process.exit(1)});
''')
        self.assertEqual(result.returncode,0,result.stderr)

    def test_atomic_password_write_checks_set_and_commit(self):
        source=r'''
#include <cassert>
#include <cstring>
#include <cstdint>
#include <cstddef>

#include <cstddef>
#define ESP_OK 0
#define ESP_FAIL -1
#define NVS_READWRITE 1
#define HTTPD_403_FORBIDDEN 403
#define HTTPD_400_BAD_REQUEST 400
#define ADMIN_PASSWORD_MIN_LEN 8
#define ADMIN_PASSWORD_MAX_LEN 64
#define ADMIN_PASSWORD_PBKDF2_ITERATIONS 4096
#define ESP_LOGI(...) ((void)0)
typedef int esp_err_t;
typedef int nvs_handle_t;
struct httpd_req_t {int content_len;};
struct admin_credentials_t {uint32_t iterations;uint8_t salt[16];uint8_t hash[32];};
static uint8_t s_admin_pw_salt[16],s_admin_pw_hash[32];
static uint32_t s_admin_pw_iterations;
static bool s_admin_password_set;
static int set_error,commit_error,commits,status;
static bool authorized=true;
static bool authorize_mutation(httpd_req_t*){return authorized;}
static bool reject_unless_own_page(httpd_req_t*){return true;}
static void httpd_resp_send_err(httpd_req_t*,int s,const char*){status=s;}
static void httpd_resp_send_500(httpd_req_t*){status=500;}
static void httpd_resp_set_type(httpd_req_t*,const char*){}
static int httpd_resp_send(httpd_req_t*,const char*,int){return 0;}
static int httpd_req_recv(httpd_req_t*,char*p,int n){memcpy(p,"newpassword",n);return n;}
static void esp_fill_random(uint8_t*p,size_t n){memset(p,5,n);}
static bool hash_admin_password(const char*,size_t,const uint8_t*,uint32_t,uint8_t*h){memset(h,7,32);return true;}
static int nvs_open(const char*,int,int*h){*h=1;return 0;}
static int nvs_set_blob(int,const char*key,const void*,size_t size){assert(!strcmp(key,"credentials"));assert(size==sizeof(admin_credentials_t));return set_error;}
static int nvs_commit(int){++commits;return commit_error;}
static void nvs_close(int){}
'''
        source+=function('main/diag_webserver.cpp','static esp_err_t admin_write_password(')
        source+=r'''
int main(){
 httpd_req_t req={11};
 set_error=1;assert(admin_write_password(&req,false)==-1);assert(!s_admin_password_set && commits==0);
 set_error=0;commit_error=1;assert(admin_write_password(&req,false)==-1);assert(!s_admin_password_set);
 commit_error=0;assert(admin_write_password(&req,false)==0);assert(s_admin_password_set && s_admin_pw_hash[0]==7);
 s_admin_pw_hash[0]=8;set_error=1;assert(admin_write_password(&req,true)==-1);assert(s_admin_pw_hash[0]==8);
 authorized=false;set_error=0;int old=commits;assert(admin_write_password(&req,true)==-1);assert(commits==old);
}
'''
        run_c(source,cpp=True)

    def test_credentials_reload_and_corrupt_storage_fail_closed(self):
        source=r'''
#include <cassert>
#include <cstring>
#include <cstdint>
#include <cstddef>

#include <cstddef>
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 2
#define NVS_READWRITE 1
typedef int nvs_handle_t;
typedef int esp_err_t;
struct admin_credentials_t {uint32_t iterations;uint8_t salt[16];uint8_t hash[32];};
static uint8_t s_admin_pw_salt[16],s_admin_pw_hash[32];
static uint32_t s_admin_pw_iterations;
static bool s_admin_password_set;
static admin_credentials_t durable={4096,{5},{7}};
static int blob_result,legacy_result=2;
static int nvs_open(const char*,int,int*h){*h=1;return 0;}
static void nvs_close(int){}
static int nvs_get_blob(int,const char*key,void*out,size_t*n){
 if(!strcmp(key,"credentials")){if(!blob_result)memcpy(out,&durable,sizeof(durable));return blob_result;}
 if(!legacy_result)memset(out,9,*n);return legacy_result;
}
static int nvs_get_u32(int,const char*,uint32_t*out){*out=1234;return legacy_result;}
'''
        source+=r'''
#define ESP_FAIL -1
#define HTTPD_403_FORBIDDEN 403
#define HTTPD_401_UNAUTHORIZED 401
#define ADMIN_PASSWORD_MAX_LEN 64
struct httpd_req_t {const char *token;};
static void httpd_resp_send_err(httpd_req_t*,int,const char*){}
static size_t httpd_req_get_hdr_value_len(httpd_req_t*r,const char*){return strlen(r->token);}
static int httpd_req_get_hdr_value_str(httpd_req_t*r,const char*,char*out,size_t n){strncpy(out,r->token,n);return 0;}
static bool hash_admin_password(const char*pw,size_t,const uint8_t*,uint32_t,uint8_t*out){memset(out,0,32);out[0]=!strcmp(pw,"newpassword")?7:8;return true;}
static bool constant_time_equal(const uint8_t*a,const uint8_t*b,size_t n){return !memcmp(a,b,n);}
'''
        source+=function('main/diag_webserver.cpp','static bool authorize_mutation(')+'\n'
        source+=function('main/diag_webserver.cpp','static bool init_admin_credentials(')
        source+=r'''
int main(){
 assert(init_admin_credentials()); assert(s_admin_password_set && s_admin_pw_hash[0]==7 && s_admin_pw_iterations==4096);
 // Simulate reboot: all RAM cleared, atomic persisted blob remains.
 memset(s_admin_pw_hash,0,32);s_admin_password_set=false;s_admin_pw_iterations=0;
 assert(init_admin_credentials());assert(s_admin_password_set && s_admin_pw_hash[0]==7);
 httpd_req_t old_password={"oldpassword"},new_password={"newpassword"};
 assert(!authorize_mutation(&old_password));assert(authorize_mutation(&new_password));
 blob_result=1;assert(!init_admin_credentials()); // storage read error never enables enrollment
 blob_result=2;legacy_result=0;assert(init_admin_credentials());assert(s_admin_password_set && s_admin_pw_hash[0]==9);
 legacy_result=2;assert(init_admin_credentials());assert(!s_admin_password_set);
 blob_result=0;durable.iterations=0;assert(!init_admin_credentials());
}
'''
        run_c(source,cpp=True)

    def test_rollback_handler_rejects_durable_unconfirmed_slot(self):
        source=r'''
#include <cassert>
#include <cstdint>
#include <cstddef>

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NVS_NOT_FOUND 2
#define NVS_READWRITE 1
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_RESP_USE_STRLEN -1
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define tskIDLE_PRIORITY 0
typedef int nvs_handle_t;
typedef int esp_err_t;
struct esp_partition_t {const char *label;};
struct esp_app_desc_t {char version[32];};
struct httpd_req_t {};
static bool s_ota_upload_in_progress;
static int boot_calls,restart_calls,status;
static uint8_t blocked=1;
static esp_partition_t other={"ota_1"};
static bool authorize_mutation(httpd_req_t*){return true;}
static int nvs_open(const char*,int,int*h){*h=1;return 0;}
static int nvs_get_u8(int,const char*,uint8_t*out){*out=blocked;return 0;}
static void nvs_close(int){}
static void httpd_resp_set_status(httpd_req_t*,const char*){}
static void httpd_resp_set_type(httpd_req_t*,const char*){}
static int httpd_resp_send(httpd_req_t*,const char*,int){return 0;}
static void httpd_resp_send_err(httpd_req_t*,int s,const char*){status=s;}
static void httpd_resp_send_500(httpd_req_t*){status=500;}
static const esp_partition_t*esp_ota_get_next_update_partition(void*){return &other;}
static int esp_ota_get_partition_description(const esp_partition_t*,esp_app_desc_t*){return 0;}
static int esp_ota_set_boot_partition(const esp_partition_t*){++boot_calls;return 0;}
static void delayed_restart_task(void*){}
static void xTaskCreate(void(*)(void*),const char*,int,void*,int,void*){++restart_calls;}
'''
        source+=function('main/diag_webserver.cpp','static bool ota_slot_eligible(')+'\n'
        source+=function('main/diag_webserver.cpp','static esp_err_t firmware_rollback_post_handler(')
        source+=r'''
int main(){
 httpd_req_t req;
 // Reboot has reset RAM pending state, but the NVS per-slot flag still blocks.
 assert(firmware_rollback_post_handler(&req)==-1);assert(status==400 && boot_calls==0 && restart_calls==0);
 blocked=0;assert(firmware_rollback_post_handler(&req)==0);assert(boot_calls==1 && restart_calls==1);
}
'''
        run_c(source,cpp=True)
