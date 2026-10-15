"""Real two-carrier IQ -> independent DATV demod -> FFmpeg/WebRTC integration.

Only this test's processes are controlled. Physical radios are never enumerated.
Requires playwright and psutil. --duration 600 measures a ten-minute steady run;
--duration 3600 performs a one-hour synthetic soak, following isolation checks.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import tempfile
import time
import urllib.request
import psutil
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-windows"
APP = BUILD / "SDR-Tiouner-windows-x64/SDR-Tiouner.exe"
URL = "http://127.0.0.1:18090"


def state():
    with urllib.request.urlopen(URL + "/api/state", timeout=3) as response:
        return json.load(response)


def wait_for(predicate, timeout=90):
    deadline = time.monotonic() + timeout
    latest = None
    while time.monotonic() < deadline:
        latest = state()
        if predicate(latest):
            return latest
        time.sleep(.3)
    if latest: latest.pop("spectrum", None)
    raise AssertionError("Receiver state did not settle: " + json.dumps(latest))


def main(duration, both_s2=False):
    assert APP.is_file(), "Build with bash tools/package/make_multi_windows.sh"
    with tempfile.TemporaryDirectory(prefix="OnAir multi IQ test ", ignore_cleanup_errors=True) as temporary:
        temporary = Path(temporary)
        config = (ROOT / "multi-receiver.ini").read_text(encoding="utf-8")
        config = config.replace("http_port=47321", "http_port=18090").replace("video_port_base=18900", "video_port_base=19000").replace("rtsp_port_base=18560", "rtsp_port_base=18600").replace("ice_port_base=18190", "ice_port_base=18200")
        config_path = temporary / "multi-receiver.ini"
        config_path.write_text(config, encoding="utf-8")
        log = (BUILD / "multi-receiver-test.log").open("w", encoding="utf-8")
        app = subprocess.Popen([str(APP), "--demo-s2" if both_s2 else "--demo", "--no-open", "--config", str(config_path)], cwd=ROOT, stdout=log, stderr=log, creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        try:
            for _ in range(100):
                assert app.poll() is None, "PoC exited; see multi-receiver-test.log"
                try:
                    state()
                    break
                except OSError:
                    time.sleep(.2)
            else:
                raise AssertionError("Local web API did not start")
            with sync_playwright() as pw:
                browser = pw.chromium.launch(executable_path="C:/Program Files/Google/Chrome/Application/chrome.exe", headless=True)
                page = browser.new_page(viewport={"width": 1440, "height": 1100})
                errors = []
                page.on("pageerror", lambda error: errors.append(str(error)))
                page.goto(URL)
                page.wait_for_selector('[data-channel="2"]')
                statuses = wait_for(lambda s: len(s["channels"]) == 2 and all(ch["locked"] and ch["packets"] > 100 for ch in s["channels"]))
                assert {ch["standard"] for ch in statuses["channels"]} == ({2} if both_s2 else {1,2})
                print("PASS: two simultaneous DVB-S/S2 carriers demodulated from one summed wideband IQ stream", flush=True)
                for channel_id in (1, 2):
                    page.wait_for_function("id=>{const v=document.querySelector('[data-channel=\"'+id+'\"] video');return v.videoWidth>0&&v.currentTime>1;}", arg=channel_id, timeout=60000)
                    video = page.locator(f'[data-channel="{channel_id}"] video')
                    bright = video.evaluate("v=>{const c=document.createElement('canvas');c.width=160;c.height=90;const x=c.getContext('2d');x.drawImage(v,0,0,160,90);const d=x.getImageData(0,0,160,90).data;let n=0;for(let i=0;i<d.length;i+=4)if(d[i]+d[i+1]+d[i+2]>180)n++;return n;}")
                    assert bright > 500, f"Channel {channel_id} is black"
                    assert video.evaluate("v=>v.srcObject.getAudioTracks().length") == 1
                assert not errors, errors
                page.screenshot(path=str(BUILD / "multi-receiver-live.png"), full_page=True)
                print("PASS: both decoded colour pictures and audio tracks reach Chrome", flush=True)
                assert page.locator('#batc-panel').bounding_box()['y'] < page.locator('#radio').bounding_box()['y']
                assert 'batcSpectrum' in state()
                # Receiver spectrum is passive, and gain mode changes do not retune channels.
                revisions=[c['revision'] for c in state()['channels']]
                page.locator('#spectrum').click(position={'x':420,'y':100})
                page.wait_for_timeout(400)
                assert [c['revision'] for c in state()['channels']]==revisions
                assert page.locator('#gain-mode').input_value()=='auto'
                assert page.locator('#gain').is_disabled()
                page.locator('#gain-mode').select_option('manual')
                wait_for(lambda s: not s['autoGain'])
                page.locator('#gain').fill('42')
                page.locator('#gain-apply').click()
                wait_for(lambda s: s['gain']==42)
                assert [c['revision'] for c in state()['channels']]==revisions
                page.locator('#gain-mode').select_option('auto')
                wait_for(lambda s: s['autoGain'])
                assert page.locator('#gain').is_disabled()
                print('PASS: only BATC tunes; Auto/Manual gain applies without channel resets',flush=True)
                # A beacon click must address only the selected dynamic channel.
                beacon_b_track=page.locator('[data-channel="2"] video').evaluate("v=>v.srcObject.getVideoTracks()[0].id")
                page.locator('#target').select_option('1')
                rect=page.locator('#batc-spectrum').bounding_box()
                current=state()
                page.locator('#batc-spectrum').click(position={'x':14+(10491.5-(current['centerMhz']-current['sampleRate']/2e6))/(current['sampleRate']/1e6)*(rect['width']-28),'y':80})
                wait_for(lambda s: s['channels'][0]['frequency']==10491.5 and s['channels'][0]['locked'])
                assert page.locator('[data-channel="2"] video').evaluate("v=>v.srcObject.getVideoTracks()[0].id")==beacon_b_track
                print('PASS: BATC panel is at top; click tunes selected channel and preserves the other',flush=True)
                page.screenshot(path=str(BUILD/'multi-batc-live.png'),full_page=True)
                a = page.locator('[data-channel="1"]')
                b = page.locator('[data-channel="2"] video')
                before_time = b.evaluate("v=>v.currentTime")
                before_track = b.evaluate("v=>v.srcObject.getVideoTracks()[0].id")
                before_packets = state()["channels"][1]["packets"]
                a.locator('.stop').click()
                wait_for(lambda s: not s["channels"][0]["enabled"])
                page.wait_for_timeout(2000)
                assert b.evaluate("v=>v.currentTime") > before_time + 1
                assert b.evaluate("v=>v.srcObject.getVideoTracks()[0].id") == before_track
                assert state()["channels"][1]["packets"] > before_packets
                a.locator('.frequency').fill('10492.250')
                a.locator('.start').click()
                wait_for(lambda s: s["channels"][0]["frequency"] == 10492.25 and s["channels"][0]["enabled"])
                page.wait_for_timeout(2500)
                assert b.evaluate("v=>v.srcObject.getVideoTracks()[0].id") == before_track
                assert state()["channels"][1]["locked"]
                a.locator('.frequency').fill('10491.500')
                a.locator('.start').click()
                wait_for(lambda s: s["channels"][0]["locked"])
                page.wait_for_function("document.querySelector('[data-channel=\"1\"] video').currentTime>1", timeout=60000)
                assert b.evaluate("v=>v.srcObject.getVideoTracks()[0].id") == before_track
                print("PASS: stop/retune/reacquire A preserves B lock, packets and original WebRTC track", flush=True)
                # Kill only a helper belonging to this test's channel A. Supervisor must recover it.
                app_process = psutil.Process(app.pid)
                encoders = [child for child in app_process.children(recursive=True) if child.name().lower() == 'ffmpeg.exe']
                encoder_a = next(child for child in encoders if URL + '/stream/1' in child.cmdline())
                track_a = a.locator('video').evaluate("v=>v.srcObject.getVideoTracks()[0].id")
                encoder_a.kill()
                page.wait_for_timeout(2500)
                assert state()["channels"][1]["locked"]
                assert b.evaluate("v=>v.srcObject.getVideoTracks()[0].id") == before_track
                page.wait_for_function("old=>{const v=document.querySelector('[data-channel=\"1\"] video');return v.currentTime>1&&v.srcObject?.getVideoTracks()[0]?.id!==old;}",arg=track_a,timeout=60000)
                print("PASS: failed A encoder recovers independently; B track stays unchanged", flush=True)
                response = page.request.post(URL + '/api/channel?id=1&frequency=10491.5&symbolRate=1500&standard=0&enabled=1')
                assert response.status == 403
                token = page.evaluate('token')
                response = page.request.post(URL + '/api/channel?id=1&frequency=NaN&symbolRate=1500&standard=0&enabled=1', headers={"Origin": URL,"X-OnAir-Control":token})
                assert response.status == 400
                response = page.request.post(URL + '/api/channel/create?frequency=10494.5&symbolRate=500&standard=0&enabled=0', headers={"Origin":URL,"X-OnAir-Control":token})
                assert response.status == 202
                page.wait_for_timeout(500)
                assert len(state()["channels"]) == 2
                page.set_viewport_size({"width":390,"height":844})
                page.wait_for_timeout(300)
                assert page.evaluate('document.documentElement.scrollWidth<=innerWidth')
                page.screenshot(path=str(BUILD / "multi-receiver-mobile.png"),full_page=True)
                page.set_viewport_size({"width":1440,"height":1100})
                assert not errors, errors
                print("PASS: channel cap, control-token validation, invalid input and mobile layout",flush=True)
                page.wait_for_timeout(10000) # settle acquisition and reconnect work before the steady run
                drop_baseline = [ch["droppedSamples"] for ch in state()["channels"]]
                samples = []
                previous_cpu = {}; previous_time = time.monotonic()
                start_time = previous_time
                # Warm up process CPU baselines, including both managed servers and encoders.
                def cpu_memory(processes, label, elapsed):
                    total_cpu = 0; rss = 0
                    for process in processes:
                        try:
                            timing = process.cpu_times(); cpu = timing.user + timing.system
                            rss += process.memory_info().rss
                            key = (label,process.pid)
                            total_cpu += max(0,cpu-previous_cpu.get(key,cpu)); previous_cpu[key]=cpu
                        except (psutil.NoSuchProcess,psutil.AccessDenied):
                            pass
                    return total_cpu/max(.001,elapsed)/psutil.cpu_count()*100,rss/1048576
                cpu_memory([app_process]+app_process.children(recursive=True),'backend',1)
                while time.monotonic()-start_time < duration:
                    page.wait_for_timeout(1000)
                    now = time.monotonic(); elapsed=now-previous_time; previous_time=now
                    backend=[app_process]+app_process.children(recursive=True)
                    cpu,ram=cpu_memory(backend,'backend',elapsed)
                    chrome=[p for p in psutil.Process().children(recursive=True) if p.name().lower()=='chrome.exe']
                    browser_cpu,browser_ram=cpu_memory(chrome,'browser',elapsed)
                    current=state()
                    assert all(ch['locked'] for ch in current['channels']), current['channels']
                    assert current['sourceDrops']==0
                    assert [ch['droppedSamples'] for ch in current['channels']] == drop_baseline, current['channels']
                    for video in (a.locator('video'),b):
                        assert video.evaluate("v=>v.readyState>=2&&v.srcObject?.getVideoTracks()[0]?.readyState==='live'")
                    samples.append({'seconds':now-start_time,'backend_cpu_pct_whole_pc':cpu,'backend_ram_mb':ram,'browser_cpu_pct_whole_pc':browser_cpu,'browser_ram_mb':browser_ram,'packets':[ch['packets']for ch in current['channels']]})
                    if len(samples)%30==0:
                        print(f"Soak {now-start_time:.0f}/{duration}s: backend+helpers {cpu:.1f}% whole CPU, {ram:.1f} MiB RAM; Chrome {browser_ram:.1f} MiB",flush=True)
                result={'duration_seconds':duration,'synthetic_rf':True,'physical_radio_tested':False,'samples':samples,'backend_cpu_mean_pct':statistics.mean(s['backend_cpu_pct_whole_pc']for s in samples),'backend_ram_peak_mb':max(s['backend_ram_mb']for s in samples),'combined_ram_peak_mb':max(s['backend_ram_mb']+s['browser_ram_mb']for s in samples)}
                result['both_dvbs2']=both_s2
                result['backend_cpu_within_target']=result['backend_cpu_mean_pct']<25
                result['backend_ram_within_target']=result['backend_ram_peak_mb']*1048576<500000000
                mode='s2-s2' if both_s2 else 's2-s'
                (BUILD/f'multi-receiver-measurements-{mode}-{duration}s.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
                (BUILD/'multi-receiver-measurements.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
                print('RESULT:',json.dumps({key:value for key,value in result.items()if key!='samples'}),flush=True)
                assert not errors,errors
                page.locator('#quit').click()
                app.wait(timeout=15)
                assert app.returncode==0
                browser.close()
        finally:
            if app.poll() is None:
                owned_helpers = psutil.Process(app.pid).children(recursive=True)
                try:
                    page_text = urllib.request.urlopen(URL,timeout=2).read().decode('utf-8')
                    control = page_text.split("const token='")[1].split("'")[0]
                    request = urllib.request.Request(URL+'/api/shutdown',method='POST',headers={'Origin':URL,'X-OnAir-Control':control})
                    urllib.request.urlopen(request,timeout=2).close()
                    app.wait(timeout=15)
                except Exception:
                    if app.poll() is None:
                        app.terminate();app.wait(timeout=15)
                psutil.wait_procs(owned_helpers,timeout=3)
            log.close()
            cache=temporary/'multi-cache'
            if cache.is_dir():
                output=BUILD/'multi-test-logs';output.mkdir(exist_ok=True)
                for source in cache.rglob('browser-video.log'):
                    shutil.copyfile(source,output/(source.parent.name+'.log'))


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--duration',type=int,default=60);parser.add_argument('--s2',action='store_true')
    args=parser.parse_args();assert args.duration>0
    main(args.duration,args.s2)
