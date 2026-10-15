#pragma once
// Shared by both pages. The backend lease also coordinates separate browsers.
static const char* playbackScript = R"PLAYBACK(
let playbackActive=false;
// randomUUID is unavailable on plain HTTP LAN origins; getRandomValues works there.
const playbackClient=Array.from(crypto.getRandomValues(new Uint8Array(16)),b=>b.toString(16).padStart(2,'0')).join('');
let playbackGeneration=0,playbackStopped=false,playbackTimer=null;
const playbackBroadcast=typeof BroadcastChannel!=='undefined'?new BroadcastChannel('onair-playback'):null;
if(playbackBroadcast)playbackBroadcast.onmessage=e=>{if(e.data!==playbackClient){++playbackGeneration;pausePlayback();}};
function pausePlayback(){playbackActive=false;for(const v of views.values())if(v.pc||v.busy)disconnect(v);}
async function playbackRequest(action){
 const generation=++playbackGeneration;
 if(action==='claim')pausePlayback();
 try{
  const page=location.pathname==='/monitor'?'monitor':'receiver';
  const r=await fetch('/api/playback/'+action+'?client='+playbackClient+'&page='+page,{method:'POST',headers:{'X-OnAir-Control':token},signal:AbortSignal.timeout(3000)});
  if(!r.ok)throw Error('Playback control unavailable');
  const result=await r.json();if(generation!==playbackGeneration||playbackStopped)return;
  playbackActive=result.active;
  if(action==='claim'&&playbackActive&&playbackBroadcast)playbackBroadcast.postMessage(playbackClient);
  if(!playbackActive)pausePlayback();
  const banner=document.querySelector('#playback-notice');banner.hidden=playbackActive;
  document.querySelector('#playback-text').textContent=result.page?'Video is active on the '+(result.page==='monitor'?'Auto Monitor':'Receiver')+' page.':'Video playback is paused.';
 }catch(e){if(generation===playbackGeneration){pausePlayback();document.querySelector('#playback-notice').hidden=false;document.querySelector('#playback-text').textContent='Video paused: unable to confirm playback ownership.';}}
}
function startPlaybackControl(){
 const banner=document.createElement('div');banner.id='playback-notice';banner.setAttribute('role','status');banner.style.cssText='padding:12px;margin:12px;border:1px solid #506887;border-radius:8px;background:#172638;color:#e5edf8';
 const text=document.createElement('span');text.id='playback-text';text.textContent='Connecting video…';
 const button=document.createElement('button');button.textContent='Play video here';button.style.cssText='margin-left:12px;padding:6px 12px';button.onclick=()=>playbackRequest('claim');banner.append(text,button);document.body.prepend(banner);
 playbackRequest('claim');playbackTimer=setInterval(()=>playbackRequest('heartbeat'),1000);
}
window.addEventListener('pagehide',()=>{playbackStopped=true;++playbackGeneration;clearInterval(playbackTimer);pausePlayback();fetch('/api/playback/release?client='+playbackClient,{method:'POST',headers:{'X-OnAir-Control':token},keepalive:true}).catch(()=>{});});
window.addEventListener('pageshow',e=>{if(e.persisted){playbackStopped=false;playbackRequest('claim');playbackTimer=setInterval(()=>playbackRequest('heartbeat'),1000);}});
document.addEventListener('DOMContentLoaded',startPlaybackControl);
)PLAYBACK";
