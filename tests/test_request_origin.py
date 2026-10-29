"""Host tests for the same-origin check on the unauthenticated set-password endpoint."""
import unittest
from test_regressions import ROOT, function, run_c

INC = ('#include <assert.h>\n#include <string.h>\n'
       '#include "' + str(ROOT / 'main/request_origin.h') + '"\n')


class RequestOriginTests(unittest.TestCase):
    def test_host_must_be_ip_literal_or_mdns_name(self):
        run_c(INC + r'''
int main(void) {
 assert(request_host_is_local("192.168.1.168:8080"));
 assert(request_host_is_local("192.168.4.1"));
 assert(request_host_is_local("gdo-blaq.local:8080"));
 assert(request_host_is_local("GDO-BLAQ.LOCAL."));
 assert(request_host_is_local("[fe80::1%25en0]:8080"));
 assert(!request_host_is_local("attacker.example:8080"));   /* DNS rebinding */
 assert(!request_host_is_local("garage.lan"));
 assert(!request_host_is_local("local"));
 assert(!request_host_is_local(".local"));
 assert(!request_host_is_local("192.168.1.256"));
 assert(!request_host_is_local("192.168.1"));
 assert(!request_host_is_local("1.2.3.4.5"));
 assert(!request_host_is_local(""));
 assert(!request_host_is_local(NULL));
 return 0;
}''')

    def test_origin_must_match_host_exactly(self):
        run_c(INC + r'''
int main(void) {
 assert(request_origin_matches_host("http://192.168.1.168:8080", "192.168.1.168:8080"));
 assert(request_origin_matches_host("http://GDO-BLAQ.local:8080/", "gdo-blaq.local:8080"));
 assert(!request_origin_matches_host("http://192.168.1.168", "192.168.1.168:8080"));  /* port differs */
 assert(!request_origin_matches_host("http://192.168.1.168:8080", "192.168.1.169:8080"));
 assert(!request_origin_matches_host("null", "192.168.1.168:8080"));
 assert(!request_origin_matches_host("192.168.1.168:8080", "192.168.1.168:8080"));   /* no scheme */
 assert(!request_origin_matches_host("http://192.168.1.168:8080.evil.example", "192.168.1.168:8080"));
 assert(!request_origin_matches_host("", "192.168.1.168:8080"));
 return 0;
}''')

    def test_browser_cross_site_post_is_refused_and_curl_is_not(self):
        run_c(INC + r'''
int main(void) {
 const char *H = "192.168.1.168:8080";
 /* The dashboard itself (fetch from the same page). */
 assert(request_from_own_page(H, "http://192.168.1.168:8080", "same-origin"));
 assert(request_from_own_page(H, "http://192.168.1.168:8080", ""));
 /* Typed into the address bar / older browser: no Origin, maybe no Sec-Fetch-Site. */
 assert(request_from_own_page(H, "", "none"));
 assert(request_from_own_page(H, NULL, NULL));
 /* curl / scripts. */
 assert(request_from_own_page("gdo-blaq.local:8080", NULL, NULL));
 /* A page on another site posting text/plain (no preflight). */
 assert(!request_from_own_page(H, "http://evil.example", "cross-site"));
 assert(!request_from_own_page(H, "http://evil.example", ""));
 assert(!request_from_own_page(H, "", "cross-site"));
 assert(!request_from_own_page(H, "null", ""));
 /* DNS rebinding: Origin and Host agree, but neither names this device. */
 assert(!request_from_own_page("evil.example:8080", "http://evil.example:8080", "same-origin"));
 return 0;
}''')

    def test_set_password_handler_checks_origin_before_reading_the_body(self):
        fn = (function('main/diag_webserver.cpp', 'static esp_err_t admin_write_password(') +
              function('main/diag_webserver.cpp', 'static esp_err_t admin_set_password_post_handler('))
        helpers = (function('main/diag_webserver.cpp', 'static int copy_req_header(') +
                   function('main/diag_webserver.cpp', 'static bool reject_unless_own_page('))
        run_c(INC + r'''
#include <stdint.h>
#include <stdbool.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_403_FORBIDDEN 403
#define ADMIN_PASSWORD_MIN_LEN 8
#define ADMIN_PASSWORD_MAX_LEN 64
#define ADMIN_PASSWORD_PBKDF2_ITERATIONS 4096
#define TAG "t"
#define NVS_READWRITE 1
typedef int esp_err_t;
typedef int nvs_handle_t;
struct httpd_req_t { const char *host, *origin, *site, *body; int content_len, pos, status; };
struct admin_credentials_t { uint32_t iterations; uint8_t salt[16]; uint8_t hash[32]; };
static bool authorize_mutation(httpd_req_t *) { assert(false); return false; }
static uint8_t s_admin_pw_salt[16], s_admin_pw_hash[32];
static uint32_t s_admin_pw_iterations;
static bool s_admin_password_set, kdf_ok = true;
static int bodies_read, commits;
static size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *n) {
 const char *v = !strcmp(n,"Host") ? r->host : !strcmp(n,"Origin") ? r->origin : r->site;
 return v ? strlen(v) : 0; }
static int httpd_req_get_hdr_value_str(httpd_req_t *r, const char *n, char *b, size_t sz) {
 const char *v = !strcmp(n,"Host") ? r->host : !strcmp(n,"Origin") ? r->origin : r->site;
 strncpy(b, v, sz); b[sz-1] = 0; return ESP_OK; }
static void httpd_resp_send_err(httpd_req_t *r, int s, const char *) { r->status = s; }
static void httpd_resp_send_500(httpd_req_t *r) { r->status = 500; }
static void httpd_resp_set_type(httpd_req_t *, const char *) {}
static int httpd_resp_send(httpd_req_t *r, const char *, int) { r->status = 200; return ESP_OK; }
static int httpd_req_recv(httpd_req_t *r, char *buf, int n) {
 ++bodies_read; int left = r->content_len - r->pos; if (n > left) n = left;
 memcpy(buf, r->body + r->pos, n); r->pos += n; return n; }
static void esp_fill_random(void *p, size_t n) { memset(p, 0x5a, n); }
static bool hash_admin_password(const char *pw, size_t len, const uint8_t *salt, uint32_t it, uint8_t *out) {
 memset(out, 0, 32); for (size_t i = 0; i < len && i < 32; ++i) out[i] = (uint8_t)pw[i] ^ salt[i % 16]; return kdf_ok; }
static int nvs_open(const char *, int, nvs_handle_t *h) { *h = 1; return ESP_OK; }
static int nvs_set_blob(nvs_handle_t, const char *, const void *, size_t) { return ESP_OK; }
static int nvs_set_u32(nvs_handle_t, const char *, uint32_t) { return ESP_OK; }
static int nvs_commit(nvs_handle_t) { ++commits; return ESP_OK; }
static void nvs_close(nvs_handle_t) {}
''' + helpers + fn + r'''
static int post(const char *host, const char *origin, const char *site, const char *body) {
 httpd_req_t r = {host, origin, site, body, (int)strlen(body), 0, 0};
 admin_set_password_post_handler(&r); return r.status; }
int main() {
 /* An oversized header is a rejection, not an absent header. */
 static char longorigin[200], longhost[200];
 memset(longorigin, 'a', sizeof(longorigin) - 1);
 memset(longhost, 'b', sizeof(longhost) - 1);
 assert(post("192.168.1.168:8080", longorigin, NULL, "attacker-pw") == 403);
 assert(post(longhost, NULL, NULL, "attacker-pw") == 403);
 assert(post("192.168.1.168:8080", NULL, longorigin, "attacker-pw") == 403);
 assert(bodies_read == 0 && commits == 0 && !s_admin_password_set);
 /* A page on another LAN host trying to claim a fresh device. */
 assert(post("192.168.1.168:8080", "http://evil.example", "cross-site", "attacker-pw") == 403);
 assert(bodies_read == 0 && commits == 0 && !s_admin_password_set);
 assert(post("evil.example:8080", "http://evil.example:8080", "same-origin", "attacker-pw") == 403);
 assert(bodies_read == 0 && commits == 0);
 /* The device's own page, and a plain curl. */
 kdf_ok = false;
 assert(post("192.168.1.168:8080", "http://192.168.1.168:8080", "same-origin", "correct-horse") == 500);
 assert(commits == 0 && !s_admin_password_set);   /* no uninitialised hash stored */
 kdf_ok = true;
 assert(post("gdo-blaq.local:8080", NULL, NULL, "correct-horse") == 200);
 assert(commits == 1 && s_admin_password_set && s_admin_pw_iterations == 4096);
 assert(post("192.168.1.168:8080", "http://192.168.1.168:8080", "same-origin", "another-pw") == 403);
 assert(commits == 1);
}
''', cpp=True)


if __name__ == '__main__':
    unittest.main()
