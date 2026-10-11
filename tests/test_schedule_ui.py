"""Node-executed tests for the schedule helpers in the diagnostics page script."""
import json
import re
import subprocess
import unittest
from test_regressions import ROOT


def page_script():
    source = (ROOT / 'main/diag_webserver.cpp').read_text()
    section = source[source.index('"<script>"'):source.index('"</script>"')]
    page = ''.join(json.loads(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', section))
    script = page.split('<script>')[1].split('</script>')[0]
    return script.replace('refresh();setInterval(', 'setInterval(')


HARNESS = r'''
const assert = require('assert');
const elements = new Map();
global.setInterval=()=>{};
global.setTimeout=(f)=>f();
global.document={getElementById(id){if(!elements.has(id)) elements.set(id,{textContent:'',checked:false,style:{},value:'',focus(){}});return elements.get(id);},
 createElement(){return {click(){},remove(){}};},body:{appendChild(){}},addEventListener(){}};
'''


def run_node(body):
    result = subprocess.run(['node', '-e', HARNESS + page_script() + '\n' + body],
                            capture_output=True, text=True)
    return result


class ScheduleUiTests(unittest.TestCase):
    def check(self, body):
        r = run_node(body)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_suggest_timezone(self):
        self.check(r'''
const PT = TZ_MAP['America/Los_Angeles'];
assert.equal(suggestTimezone('UTC0', false, 'America/Los_Angeles'), PT);
assert.equal(suggestTimezone('UTC0', true, 'America/Los_Angeles'), '');
assert.equal(suggestTimezone('EST5EDT,M3.2.0,M11.1.0', false, 'America/Los_Angeles'), '');
assert.equal(suggestTimezone('UTC0', false, 'Mars/Olympus'), '');
assert.equal(suggestTimezone('UTC0', false, ''), '');
assert.equal(suggestTimezone('UTC0', false, 'UTC'), '');
''')

    def test_render_schedule_info(self):
        self.check(r'''
const el = document.getElementById('scInfo');
renderScheduleInfo({clock_synced:true, close_schedule_enabled:true, close_schedule_time:'23:00',
                    timezone:'PST8PDT,M3.2.0,M11.1.0', local_time:'2026-10-11 20:15:00'});
assert(el.textContent.includes('Closes at 23:00 device time'), el.textContent);
assert(el.textContent.includes('PST8PDT,M3.2.0,M11.1.0'), el.textContent);
assert(el.textContent.includes('Device time now: 2026-10-11 20:15:00'), el.textContent);
renderScheduleInfo({clock_synced:true, close_schedule_enabled:false, timezone:'UTC0', local_time:'T1'});
assert(el.textContent.includes('Device local time: T1') && el.textContent.includes('UTC0'), el.textContent);
renderScheduleInfo({clock_synced:false, close_schedule_enabled:true, timezone:'UTC0'});
assert(el.textContent.startsWith('Clock not synced yet'), el.textContent);
''')
