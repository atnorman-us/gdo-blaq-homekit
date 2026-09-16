import unittest
from test_regressions import function, run_c

class ControlTests(unittest.TestCase):
    def test_auto_close_revalidates_after_warning(self):
        fn = function('main/gdo-blaq-homekit.cpp','static void auto_close_warning_complete_cb(void)')
        run_c(r'''
#include <stdint.h>
#include <assert.h>
#define ESP_LOGI(...) ((void)0)
#define ESP_OK 0
#define GDO_DOOR_STATE_OPEN 0
#define GDO_OBSTRUCTION_STATE_CLEAR 0
#define TGT_CLOSED 1
struct ControlGuard {};
struct gdo_status_t { bool synced; int door,obstruction; uint32_t door_command_id; } current;
static int calls, last_door, last_obstruction;
static bool s_auto_close_enabled, s_auto_close_triggered;
static uint32_t s_auto_close_generation, s_close_generation, s_auto_close_command_id;
static int64_t s_door_open_since_ms;
static int64_t now_ms() {return 100;}
static int gdo_get_status(gdo_status_t *s) {*s=current; return ESP_OK;}
static int gdo_door_close() {++calls;return ESP_OK;}
static void notify_homekit_target_door_state_change(int) {}
''' + fn + r'''
static void reset() {
 calls=0; current={true,0,0,7}; last_door=last_obstruction=0;
 s_auto_close_enabled=true; s_auto_close_generation=s_close_generation=3;
 s_auto_close_command_id=7; s_auto_close_triggered=true;
}
int main() {
 reset(); auto_close_warning_complete_cb(); assert(calls==1);
 reset(); s_auto_close_enabled=false; auto_close_warning_complete_cb(); assert(calls==0);
 reset(); current.obstruction=1; auto_close_warning_complete_cb(); assert(calls==0);
 reset(); current.door=2; auto_close_warning_complete_cb(); assert(calls==0);
 reset(); current.synced=false; auto_close_warning_complete_cb(); assert(calls==0);
 reset(); ++s_close_generation; auto_close_warning_complete_cb(); assert(calls==0);
 reset(); ++current.door_command_id; auto_close_warning_complete_cb(); assert(calls==0);
}
''', cpp=True)

    def test_web_authorization_requires_valid_password(self):
        fn = function('main/diag_webserver.cpp', 'static bool authorize_mutation(')
        prefix = r'''
#include <cstring>
#include <cassert>
#include <cstddef>
#include <cstdint>
#define ESP_OK 0
#define HTTPD_401_UNAUTHORIZED 401
#define HTTPD_403_FORBIDDEN 403
#define ADMIN_PASSWORD_MAX_LEN 64
struct httpd_req_t { const char *token; int status; };
static uint8_t s_admin_pw_salt[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
static uint8_t s_admin_pw_hash[32];
static uint32_t s_admin_pw_iterations = 4096;
static bool s_admin_password_set = false;
static void httpd_resp_send_err(httpd_req_t *r,int s,const char*) {r->status=s;}
static size_t httpd_req_get_hdr_value_len(httpd_req_t*r,const char*) {return strlen(r->token);}
static int httpd_req_get_hdr_value_str(httpd_req_t*r,const char*,char*b,size_t n) {strncpy(b,r->token,n);return 0;}
// Fake but deterministic stand-in for the real PBKDF2-HMAC-SHA256 - these
// tests cover the auth flow (unset/missing/wrong/right password), not the
// KDF itself, which is a well-tested standard primitive.
static void hash_admin_password(const char *password, size_t len, const uint8_t *salt, uint32_t iterations, uint8_t *out_hash) {
 memset(out_hash, 0, 32);
 for (size_t i = 0; i < len && i < 32; ++i) out_hash[i] = (uint8_t)password[i] ^ salt[i % 16] ^ (uint8_t)iterations;
}
'''
        main = r'''
int main() {
 httpd_req_t req={"correct-horse",0};
 assert(!authorize_mutation(&req)); assert(req.status==403); // no password set yet
 s_admin_password_set = true;
 hash_admin_password("correct-horse", 13, s_admin_pw_salt, s_admin_pw_iterations, s_admin_pw_hash);
 req.token=""; assert(!authorize_mutation(&req)); assert(req.status==401);
 req.token="wrong-password"; assert(!authorize_mutation(&req)); assert(req.status==401);
 req.token="correct-horse"; assert(authorize_mutation(&req));
}
'''
        run_c(prefix+fn+main, cpp=True)
