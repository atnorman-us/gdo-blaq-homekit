"""Exercise log authorization and formatted credential messages on the host."""
import re
import unittest
from test_regressions import ROOT, function, run_c


def extract_log_calls(source):
    """Find each ESP_LOG*(...); statement by balancing parens/quotes, not
    just scanning for the next ';' - a message can legitimately contain one
    (e.g. "AP configuration unavailable; starting default AP"), which a
    naive regex up to the first ';' truncates mid-string into invalid C."""
    statements = []
    i = 0
    while True:
        m = re.search(r'ESP_LOG[IEWD]\(', source[i:])
        if not m:
            break
        start = i + m.start()
        j = start + m.end() - m.start() - 1  # index of the opening '('
        depth, in_str, esc = 0, False, False
        while j < len(source):
            c = source[j]
            if in_str:
                if esc:
                    esc = False
                elif c == '\\':
                    esc = True
                elif c == '"':
                    in_str = False
            elif c == '"':
                in_str = True
            elif c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    j += 1
                    break
            j += 1
        end = source.index(';', j) + 1
        statements.append(source[start:end])
        i = end
    return statements


class LogSecurityTests(unittest.TestCase):
    def test_log_routes_require_valid_token_before_reading_buffer(self):
        source = r'''
#include <cstring>
#include <cassert>
#include <cstddef>
#define ESP_OK 0
#define ESP_FAIL -1
#define HTTPD_403_FORBIDDEN 403
#define HTTPD_401_UNAUTHORIZED 401
#define HTTPD_RESP_USE_STRLEN -1
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
struct httpd_req_t { const char *token; int status; bool attachment; bool no_store; };
static char s_admin_token[65], scratch[64];
static char *s_log_scratch = scratch;
static int s_log_scratch_mutex = 1, reads, sends;
static size_t httpd_req_get_hdr_value_len(httpd_req_t*r,const char*) {return strlen(r->token);}
static int httpd_req_get_hdr_value_str(httpd_req_t*r,const char*,char*b,size_t n) {strncpy(b,r->token,n);return 0;}
static void httpd_resp_send_err(httpd_req_t*r,int s,const char*) {r->status=s;}
static void httpd_resp_send_500(httpd_req_t*r) {r->status=500;}
static void httpd_resp_set_type(httpd_req_t*,const char*) {}
static void httpd_resp_set_hdr(httpd_req_t*r,const char*k,const char*v) {
 if(!strcmp(k,"Content-Disposition")) r->attachment=true;
 if(!strcmp(k,"Cache-Control") && !strcmp(v,"no-store")) r->no_store=true;
}
static int httpd_resp_send(httpd_req_t*,const char*,int) {++sends;return 0;}
static size_t log_ring_buffer_capacity() {return sizeof(scratch);}
static size_t log_ring_buffer_read(char*b,size_t) {++reads;strcpy(b,"private log");return 11;}
static int xSemaphoreTake(int,int) {return 1;}
static void xSemaphoreGive(int) {}
'''
        for signature in ('static bool authorize_mutation(', 'static esp_err_t send_log_buffer(',
                          'static esp_err_t logs_get_handler(', 'static esp_err_t logs_download_get_handler('):
            source += function('main/diag_webserver.cpp', signature) + '\n'
        run_c(source + r'''
int main() {
 memset(s_admin_token,'a',64);
 char wrong[65]; memset(wrong,'b',64);wrong[64]=0;
 for(auto handler : {logs_get_handler, logs_download_get_handler}) {
  for(const char* token : {"", "short", (const char*)wrong}) {
   httpd_req_t req={token,0,false,false}; reads=sends=0;
   handler(&req); assert(req.status==401);assert(reads==0 && sends==0);
  }
  httpd_req_t req={s_admin_token,0,false,false};reads=sends=0;
  handler(&req);assert(reads==1 && sends==1);assert(req.no_store);
  assert(req.attachment==(handler==logs_download_get_handler));
 }
}
'''.replace('int main()', '#include <initializer_list>\nint main()'), cpp=True)

    def test_wifi_and_provisioning_log_messages_do_not_include_credentials(self):
        wifi = (ROOT / 'components/nvs_wifi_connect/source/nvs_wifi_connect.c').read_text()
        wifi = wifi[wifi.index('static void init_softap('):]
        provisioning = (ROOT / 'components/nvs_wifi_connect/source/nvs_wifi_connect_http_server.c').read_text()
        # Execute the real formatting expressions, including the generic NVS write error.
        statements = extract_log_calls(wifi)
        statements += [s for s in extract_log_calls(provisioning) if re.search(r'\b(key|value)\b', s)]
        run_c(r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define TAG "test"
#define ESP_LOGI(tag,...) do { char out[512]; snprintf(out,sizeof(out),__VA_ARGS__); assert(!strstr(out,"PRIVATE")); } while(0)
#define ESP_LOGE ESP_LOGI
#define ESP_LOGW ESP_LOGI
#define ESP_LOGD ESP_LOGI
int main(void) {
 char *ap_ssid="PRIVATE-AP", *ap_pass="PRIVATE-AP-PASS";
 char *sta_ssid="PRIVATE-STA", *sta_pass="PRIVATE-STA-PASS";
 char *nvs_ssid="PRIVATE-NVS", *nvs_password="PRIVATE-NVS-PASS";
 char *key="password", *value="PRIVATE-PROVISIONED-PASS";
 int ret=1;
''' + '\n'.join(statements) + '\n}')

    def test_dashboard_log_fetch_and_download_send_token_headers(self):
        import json
        import subprocess
        source = (ROOT / 'main/diag_webserver.cpp').read_text()
        page = ''.join(json.loads(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', source[source.index('static const char *page'):source.index('// Confirmed via')])) if 'static const char *page' in source else ''
        # Decode the actual embedded C string literals and execute its JavaScript.
        if not page:
            section = source[source.index('"<script>"'):source.index('"</script>"')]
            page = ''.join(json.loads(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', section))
        script = page.split('<script>')[1].split('</script>')[0]
        script = script.replace('refresh();setInterval(', 'setInterval(')
        result = subprocess.run(['node', '-e', r'''
const assert = require('assert');
const elements = new Map();
let calls=[], prompts=0, clicked=false;
global.prompt=()=>{prompts++;return 'secret-token';};
global.setInterval=()=>{};
global.setTimeout=(f)=>f();
global.document={getElementById(id){if(!elements.has(id)) elements.set(id,{textContent:'',checked:false,style:{}});return elements.get(id);},
 createElement(){return {click(){clicked=true;},remove(){}};},body:{appendChild(){}}};
global.URL={createObjectURL(){return 'blob:log';},revokeObjectURL(){}};
global.fetch=async (url,options)=>{
 if(url==='/status') throw Error('status not needed for this test');
 calls.push({url,options});return {ok:true,status:200,text:async()=>'log data',blob:async()=>({})};
};
''' + script + r'''
(async()=>{
 await refresh(true);
 assert(calls.some(c=>c.url==='/logs' && c.options?.headers['X-GDO-Token']==='secret-token'));
 calls=[];await downloadLogs();
 assert(calls.some(c=>c.url==='/logs/download' && c.options?.headers['X-GDO-Token']==='secret-token'));
 assert(clicked);
 adminToken='';calls=[];prompts=0;await refresh();
 assert.equal(prompts,0);assert.equal(calls.length,0);
})().catch(e=>{console.error(e);process.exit(1);});
'''], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
