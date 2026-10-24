import subprocess, unittest
from test_regressions import ROOT

class FirmwareMirrorTests(unittest.TestCase):
    def test_mirror_manifest_and_binary_length(self):
        js=r'''
const assert=require('assert');const fs=require('fs');
(async()=>{
 const src=fs.readFileSync(process.argv[1],'utf8');
 const {onRequestGet}=await import('data:text/javascript;base64,'+Buffer.from(src).toString('base64'));
 const env={FIRMWARE:{get:async k=>k==='current.json'?{text:async()=>JSON.stringify({version:'1.3.7.1',file:'usb.bin',ota:'ota.bin'})}:k==='ota.bin'?{body:new Uint8Array([233,1,2]),size:3,httpEtag:'test',writeHttpMetadata(h){h.set('content-type','application/octet-stream')}}:null}};
 let r=await onRequestGet({env,params:{path:['manifest.json']}});let m=await r.json();
 assert.equal(m.ota.path,'/firmware/ota.bin');assert.equal(m.builds[0].parts[0].path,'/firmware/usb.bin');
 r=await onRequestGet({env,params:{path:['ota.bin']}});assert.equal(r.headers.get('content-length'),'3');assert.equal(r.status,200);
 r=await onRequestGet({env,params:{path:['missing.bin']}});assert.equal(r.status,404);
})().catch(e=>{console.error(e);process.exit(1)});
'''
        r=subprocess.run(['node','-e',js,str(ROOT/'espweb/functions/firmware/[[path]].js')],capture_output=True,text=True)
        self.assertEqual(r.returncode,0,r.stderr)
