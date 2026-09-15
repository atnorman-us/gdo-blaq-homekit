"""Host checks compile the production C routines with hardware boundaries stubbed."""
import pathlib, subprocess, tempfile, unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]

def function(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

def run_c(source, cpp=False):
    with tempfile.TemporaryDirectory() as d:
        src = pathlib.Path(d) / ('test.cpp' if cpp else 'test.c')
        src.write_text(source)
        exe = pathlib.Path(d) / 'test'
        subprocess.run(['c++' if cpp else 'cc', '-fsanitize=address,undefined', '-g', str(src), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr

class RegressionTests(unittest.TestCase):
    def test_provisioning_rejects_oversized_tokens(self):
        parser = function('components/nvs_wifi_connect/source/nvs_wifi_connect_http_server.c', 'static esp_err_t json_to_str_parm(')
        run_c('#include <stdio.h>\n#include <string.h>\n#include <assert.h>\n'
              '#include "' + str(ROOT / 'components/nvs_wifi_connect/private_include/jsmn.h') + '"\n'
              'typedef int esp_err_t;\n#define ESP_FAIL -1\n#define ESP_OK 0\n' + parser + r'''
int main(void) {
 char key[16], value[64], input[256];
 snprintf(input,sizeof(input),"{\"name\":\"%080d\",\"msg\":\"x\"}",0);
 assert(json_to_str_parm(input,key,value) == ESP_FAIL);
 assert(json_to_str_parm("{}",key,value) == ESP_FAIL);
 assert(json_to_str_parm("{\"name\":\"a\"}",key,value) == ESP_FAIL);
 assert(json_to_str_parm("{\"name\":\"a\",\"msg\":\"b\"}",key,value) == ESP_OK);
 assert(!strcmp(key,"a") && !strcmp(value,"b"));
 return 0;
}''')

    def test_warning_failure_never_calls_completion(self):
        fn = function('main/pre_close_warning.c', 'esp_err_t pre_close_warning_run_async(')
        # The fixed API returns an error; both APIs exercise the same fault paths.
        source = r'''
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_LOGE(...) ((void)0)
#define pdPASS 1
#define tskIDLE_PRIORITY 0
static int calls, allocations, fail_alloc;
typedef struct { uint32_t duration_ms; void (*on_complete)(void); } warning_task_args_t;
static void *pvPortMalloc(size_t n) { if(fail_alloc) return NULL; allocations++; return malloc(n); }
static void vPortFree(void *p) { allocations--; free(p); }
static int xTaskCreate(void *a, const char*b,int c,void*d,int e,void*f) { return 0; }
static void warning_task(void *p) {}
static void complete(void) { calls++; }
'''
        run_c(source + fn + r'''
int main(void) {
 fail_alloc=1; pre_close_warning_run_async(5000,complete); assert(calls==0);
 fail_alloc=0; pre_close_warning_run_async(5000,complete); assert(calls==0 && allocations==0);
}''')

if __name__ == '__main__': unittest.main()
