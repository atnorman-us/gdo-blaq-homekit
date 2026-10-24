import unittest
from test_regressions import function,run_c
from test_schedule_ui import run_node

class FirmwareUrlTests(unittest.TestCase):
 def test_url_validation(self):
  run_c('#include <cstring>\n#include <cassert>\n'+function('main/diag_webserver.cpp','static bool firmware_url_valid(')+r'''
int main(){
assert(firmware_url_valid("https://example.com/ota.bin"));
assert(firmware_url_valid("https://example.com/ota.bin?token=test"));
for(const char *s:{"http://example.com/ota.bin","https:///ota.bin","https://user:pass@example.com/ota.bin","https://example.com/ota.bin#fragment","https://example.com/a\r\n.bin","https://example.com/release","https://example.com/a\\b.bin"})assert(!firmware_url_valid(s));
}
'''.replace('int main(){','#include <initializer_list>\nint main(){'),cpp=True)
 def test_custom_download_only_stages(self):
  r=run_node(r'''
(async()=>{
 adminToken='secret';global.confirm=()=>true;
 document.getElementById('fwUrl').value='https://example.com/firmware.bin';let calls=0;
 global.fetch=async(url,opts)=>{calls++;assert.equal(url,'/firmware/url');assert.equal(opts.body,'https://example.com/firmware.bin');assert.equal(opts.headers['X-GDO-Token'],'secret');return {ok:true,json:async()=>({version:'test'})};};
 await updateFromUrl();assert.equal(calls,1);assert.equal(document.getElementById('fwConfirmRow').style.display,'flex');assert(!githubUpdateBusy);
 document.getElementById('fwUrl').value='http://example.com/firmware.bin';await updateFromUrl();assert.equal(calls,1);
})().catch(e=>{console.error(e);process.exit(1)});
''')
  self.assertEqual(r.returncode,0,r.stderr)
