"""Isolated synthetic video/API check; never enumerates physical radios."""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
import psutil
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
URL = 'http://127.0.0.1:28090'

def state():
    with urllib.request.urlopen(URL+'/api/state', timeout=5) as r:
        return json.load(r)

def wait(predicate, timeout=60):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        try:
            s=state()
            if predicate(s): return s
        except OSError: pass
        time.sleep(.2)
    raise AssertionError('State did not settle')

def main():
    with tempfile.TemporaryDirectory(prefix='auto-monitor-') as tmp:
        config=Path(tmp)/'multi-receiver.ini'
        text=(ROOT/'multi-receiver.ini').read_text()
        for old,new in [('47321','28090'),('18900','28900'),('18560','28560'),('18190','28190'),('19000','29000')]:
            text=text.replace('='+old,'='+new)
        config.write_text(text)
        env=os.environ.copy()
        env['PATH']=str(ROOT/'build-windows/SDR-Tiouner-windows-x64')+';'+str(ROOT/'runtime/windows-base')+';C:/msys64/ucrt64/bin;'+env['PATH']
        env['DECT2_MUTE']='1'
        log=open(ROOT/'build-windows/auto-monitor-test.log','w')
        app=subprocess.Popen([str(ROOT/'build-windows/onair_multi.exe'),'--demo','--no-open','--config',str(config)],cwd=ROOT,env=env,stdout=log,stderr=log,creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            wait(lambda s:s['running'])
            # An arbitrary Host header must not bypass the bound-address check.
            try:
                urllib.request.urlopen(urllib.request.Request(URL+'/api/state',headers={'Host':'untrusted.example:28090'}))
                raise AssertionError('Untrusted Host accepted')
            except urllib.error.HTTPError as e: assert e.code==403
            before=wait(lambda s:all(c['locked'] for c in s['channels']))
            with sync_playwright() as pw:
                browser=pw.chromium.launch(executable_path='C:/Program Files/Google/Chrome/Application/chrome.exe',headless=True)
                page=browser.new_page(viewport={'width':1440,'height':1000})
                errors=[]
                page.on('pageerror',lambda e:errors.append(str(e)))
                page.goto(URL+'/monitor')
                page.wait_for_selector('[data-card] video')
                if not URL.startswith('http://127.0.0.1:'):
                    assert page.evaluate('isSecureContext') is False, 'LAN test must exercise plain HTTP'

                wait(lambda s:s['autoMonitor']['enabled'])
                # The beacon card first, then the scanner adds a card for the station it finds above it.
                found=wait(lambda s:len(s['autoMonitor']['cards'])==2)
                assert found['autoMonitor']['cards'][0]['beacon'] and not found['autoMonitor']['cards'][1]['beacon']
                assert abs(found['autoMonitor']['cards'][1]['frequency']-10496.5)<.05, found['autoMonitor']['cards']
                page.wait_for_function("document.querySelectorAll('[data-card]').length===2")
                assert page.locator('#start, #stop, #full').count()==0
                assert page.locator('#layout').input_value()=='4'
                assert page.locator('#layout option').all_text_contents()==['2 columns','3 columns','4 columns']
                assert page.locator('#activity-spectrum').bounding_box()['y'] < page.locator('#grid').bounding_box()['y']
                revisions0=[c['revision'] for c in state()['channels']]
                page.locator('#activity-spectrum').click(force=True)
                assert [c['revision'] for c in state()['channels']]==revisions0, 'Read-only spectrum retuned channels'
                try:
                    page.wait_for_function("[...document.querySelectorAll('video')].length===2 && [...document.querySelectorAll('video')].every(v=>v.videoWidth>0&&v.currentTime>1)",timeout=90000)
                except Exception:
                    print('Page errors:',errors,flush=True)
                    print(page.evaluate("[...views.values()].map(v=>({title:v.el.title,pc:v.pc?.connectionState,ice:v.pc?.iceConnectionState,remote:v.pc?.remoteDescription?.sdp}))"),flush=True)
                    raise
                assert page.locator('video').evaluate_all('(vs)=>vs.every(v=>v.muted)')
                page.locator('.volume').first.evaluate("e=>{e.value=60;e.dispatchEvent(new Event('input',{bubbles:true}));}")
                assert page.locator('video').evaluate_all('(vs)=>vs.filter(v=>!v.muted).length')==1
                assert abs(page.locator('video').first.evaluate('v=>v.volume')-.6)<.01
                page.locator('.volume').nth(1).evaluate("e=>{e.value=40;e.dispatchEvent(new Event('input',{bubbles:true}));}")
                assert page.locator('video').first.evaluate('v=>v.muted')
                assert page.locator('.volume').first.input_value()=='0'
                page.locator('.volume').nth(1).evaluate("e=>{e.value=0;e.dispatchEvent(new Event('input',{bubbles:true}));}")
                assert page.locator('video').evaluate_all('(vs)=>vs.every(v=>v.muted)')
                assert page.locator('.details').count()==0
                assert 'MHz' not in page.locator('[data-card] .card-header').first.inner_text()
                page.locator('#layout').select_option('4')
                assert page.locator('#grid').evaluate("e=>getComputedStyle(e).gridTemplateColumns.split(' ').length")==4
                page.locator('#layout').select_option('2')
                page.screenshot(path=str(ROOT/'build-windows/auto-monitor.png'),full_page=True)
                revisions=[c['revision'] for c in state()['channels']]
                other=browser.new_page()
                other.on('pageerror',lambda e:errors.append(str(e)))
                other.goto('http://127.0.0.1:28090/receiver')
                other.wait_for_function("[...document.querySelectorAll('video')].some(v=>v.videoWidth>0&&v.currentTime>1)",timeout=60000)
                page.wait_for_function("!playbackActive && [...document.querySelectorAll('video')].every(v=>!v.srcObject)")
                assert page.locator('#playback-notice').is_visible()
                assert [c['revision'] for c in state()['channels']]==revisions, 'Playback handoff retuned receivers'
                page.locator('#playback-notice button').click()
                other.wait_for_function("!playbackActive && [...document.querySelectorAll('video')].every(v=>!v.srcObject)")
                page.wait_for_function("playbackActive && [...document.querySelectorAll('video')].every(v=>v.videoWidth>0&&v.currentTime>1)",timeout=60000)
                time.sleep(3)
                assert other.evaluate('playbackActive') is False, 'Inactive page reclaimed ownership'
                other.close()
                assert page.evaluate('playbackActive') is True, 'Old owner release interrupted active page'
                # Commands retain the existing same-origin/token protection.
                try:
                    urllib.request.urlopen(urllib.request.Request(URL+'/api/auto/start',method='POST'))
                    raise AssertionError('Unauthorised command accepted')
                except urllib.error.HTTPError as e: assert e.code==403
                wait(lambda s:s['autoMonitor']['enabled'])
                page.close()
                time.sleep(2)
                assert state()['autoMonitor']['enabled'], 'Closing page stopped backend'
                page=browser.new_page()
                page.goto(URL+'/monitor')
                wait(lambda s:s['autoMonitor']['enabled'])
                token=re.search("const token='([^']+)'",page.content()).group(1)
                before_stop=state()
                page.evaluate("async token=>fetch('/api/auto/stop',{method:'POST',headers:{'X-OnAir-Control':token}})",token)
                after=wait(lambda s:not s['autoMonitor']['enabled'])
                fields=lambda s:[(c['id'],c['frequency'],c['symbolRate'],c['enabled'],c['revision']) for c in s['channels']]
                assert fields(before_stop)==fields(after), 'Stopping automation altered channels'
                assert config.read_text()==text, 'Automation changed saved settings'
                # While the page is open and active the scanner keeps itself running: a stopped scanner is started again.
                wait(lambda s:s['autoMonitor']['enabled'],timeout=20)
                page.reload()
                wait(lambda s:s['autoMonitor']['enabled'])
                # Manual tuning exits automatic mode before applying the manual command.
                token=re.search("const token='([^']+)'",page.content()).group(1)
                result=page.evaluate("async token=>(await fetch('/api/channel?id=1&frequency=10491.5&symbolRate=1500&standard=0&enabled=1',{method:'POST',headers:{'X-OnAir-Control':token}})).status",token)
                assert result==202
                wait(lambda s:not s['autoMonitor']['enabled'] and s['channels'][0]['enabled'])
                assert not errors,errors
                browser.close()
            print('PASS: beacon card and scan-found card, exclusive playback across pages and browser contexts, takeover, old-owner close, no retune, two decoded videos, audio, layouts, auth, auto-start, read-only spectrum, backend independence, no restore, settings and manual override')
        finally:
            children=psutil.Process(app.pid).children(recursive=True) if app.poll() is None else []
            app.terminate()
            try: app.wait(timeout=10)
            except subprocess.TimeoutExpired: app.kill();app.wait()
            _, remaining=psutil.wait_procs(children,timeout=10)
            for child in remaining:
                try: child.kill()
                except psutil.NoSuchProcess: pass
            psutil.wait_procs(remaining,timeout=5)
            log.close()

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser()
    parser.add_argument('--host',default='127.0.0.1',help='Receiver IPv4 address to exercise, including its LAN address')
    args=parser.parse_args()
    URL='http://'+args.host+':28090'
    main()
