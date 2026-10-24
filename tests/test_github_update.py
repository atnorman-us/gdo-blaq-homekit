import unittest
from test_schedule_ui import run_node

class GithubUpdateTests(unittest.TestCase):
    def test_latest_ota_is_staged_without_confirming(self):
        r = run_node(r'''
(async()=>{
 adminToken='local-secret';
 global.confirm=()=>true;
 const calls=[];
 global.fetch=async(url,opts)=>{
  calls.push([url,opts]);
  if(calls.length===1){assert(!opts);return {ok:true,json:async()=>({tag_name:'1.3.6.1',html_url:'https://github.com/example',assets:[{name:'gdo-blaq-homekit-1.3.6.1-ota.bin'}]})};}
  assert.equal(url,'/firmware/github');assert.equal(opts.body,'1.3.6.1');
  assert.equal(opts.headers['X-GDO-Token'],'local-secret');
  return {ok:true,json:async()=>({version:'1.3.6.1'})};
 };
 await updateFromGithub();
 assert.equal(calls.length,2);
 assert.equal(document.getElementById('fwConfirmRow').style.display,'flex');
 assert.equal(githubUpdateBusy,false);
})().catch(e=>{console.error(e);process.exit(1)});
''')
        self.assertEqual(r.returncode,0,r.stderr)

    def test_missing_asset_and_cancel_do_not_write_device(self):
        r = run_node(r'''
(async()=>{
 let calls=0;
 global.confirm=()=>false;
 global.fetch=async()=>{calls++;return {ok:true,json:async()=>({tag_name:'1.3.6.1',assets:[]})};};
 await updateFromGithub();assert.equal(calls,1);assert(document.getElementById('fwStatus').textContent.includes('not available'));
 global.fetch=async()=>{calls++;return {ok:true,json:async()=>({tag_name:'1.3.6.1',assets:[{name:'gdo-blaq-homekit-1.3.6.1-ota.bin'}]})};};
 await updateFromGithub();assert.equal(calls,2);assert.equal(githubUpdateBusy,false);
 assert.equal(document.getElementById('fwStatus').textContent,'Download cancelled.');
})().catch(e=>{console.error(e);process.exit(1)});
''')
        self.assertEqual(r.returncode,0,r.stderr)
