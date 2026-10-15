#pragma once

// Served from the receiver itself; no CDN, build step, or separate website to install.

namespace dect2 {

inline constexpr const char* webReceiverPage = R"ONAIR(<!doctype html>

<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">

<title>OnAir · QO-100</title><style>

:root{color-scheme:dark;font-family:system-ui,sans-serif;background:#0b1018;color:#edf2f8}*{box-sizing:border-box}body{margin:0}main{max-width:1440px;margin:auto;padding:26px 28px 32px}header{display:flex;align-items:center;justify-content:space-between;margin-bottom:24px}.brand{font-size:25px;font-weight:700;letter-spacing:-.8px}.brand span{color:#7ba3c9;font-size:12px;font-weight:500;letter-spacing:2px;margin-left:14px}.badge{font:12px ui-monospace,monospace;border:1px solid #2b394c;border-radius:20px;padding:8px 12px;color:#9baabd}.badge.lock{color:#89e5b8;border-color:#315747;background:#132a23}.panel{border:1px solid #253246;background:#101823;border-radius:12px;overflow:hidden}.heading{display:flex;align-items:center;justify-content:space-between;padding:16px 20px;border-bottom:1px solid #253246;font-size:14px;font-weight:600}.subtle{font-size:12px;color:#93a5bb;font-weight:400}.spectrum{position:relative;height:260px;padding:14px 16px 0}canvas{width:100%;height:100%;display:block;touch-action:none;cursor:crosshair}.controls{display:flex;gap:14px;align-items:end;flex-wrap:wrap;padding:16px 20px;border-top:1px solid #253246}label{display:block;font-size:11px;color:#93a5bb;letter-spacing:.3px}select{max-width:100%;width:min(360px,75vw);background:#0c131e;color:#edf2f8;border:1px solid #33445b;border-radius:6px;padding:10px;margin-top:6px}input{display:block;width:164px;margin-top:6px;background:#0c131e;border:1px solid #33445b;border-radius:6px;padding:10px 12px;font:15px ui-monospace,monospace;color:#edf2f8}input:focus{outline:2px solid #5aaae5;outline-offset:1px}button{border:0;border-radius:6px;padding:11px 24px;background:#80d6b0;color:#10231c;font-size:14px;font-weight:650;cursor:pointer}button:disabled{opacity:.45;cursor:default}.lower{display:grid;grid-template-columns:minmax(0,1fr) 260px;gap:20px;margin-top:20px}.video{aspect-ratio:16/9;background:#000;position:relative}iframe{border:0;width:100%;height:100%;display:block}.readout{padding:20px}.readout h2{font-size:12px;color:#93a5bb;margin:0 0 23px;text-transform:uppercase;letter-spacing:1.4px}.datum{margin-bottom:24px}.datum strong{display:block;font:22px ui-monospace,monospace;margin-top:7px;font-weight:500}.datum small{display:block;font-size:12px;color:#93a5bb;margin-top:7px}.note{font-size:12px;color:#8193aa;line-height:1.7;border-top:1px solid #253246;padding-top:16px}.message{font-size:12px;color:#a7b9ce;padding:12px 20px;min-height:40px}.error{color:#ffb39c}@media(max-width:760px){main{padding:16px 12px}.brand span{display:block;margin:4px 0}.lower{grid-template-columns:1fr}.readout{display:flex;gap:24px;flex-wrap:wrap}.readout h2{width:100%;margin-bottom:0}.datum{margin-bottom:0}.note{width:100%}.spectrum{height:230px}.heading{padding:14px}.controls{padding:14px;gap:10px}input{width:145px}}

</style></head><body><main>

<header><div class="brand">OnAir<span>QO-100 RECEIVER</span></div><div id="connection" class="badge">CONNECTING</div></header>

<section class="panel" style="margin-bottom:20px"><div class="controls"><label>Radio<select id="radio"></select></label><button id="radio-start" type="button">Start</button><button id="radio-stop" type="button">Stop</button><button id="radio-refresh" type="button">Refresh radios</button><span id="radio-status" class="subtle"></span></div></section>
<section class="panel"><div class="heading">BATC wideband spectrum<span id="batc" class="subtle">Connecting to BATC…</span></div>

<div class="spectrum"><canvas id="spectrum" aria-label="BATC spectrum: click a signal to tune"></canvas></div>

<form class="controls" id="tuning"><label>Satellite frequency · MHz<input id="frequency" type="number" min="10490.5" max="10499.5" step="0.001" value="10491.500" required></label><label>Symbol rate · kS/s<input id="symbolrate" type="number" min="0" max="10000" step="1" value="1500" required></label><button id="tune" type="submit">Tune</button><span class="subtle">Click a signal to tune · symbol rate is estimated</span></form>

</section><div class="lower"><section class="panel"><div class="heading">Live picture<span id="channel" class="subtle">Waiting for reception</span></div><div class="video"><iframe id="player" title="Live DATV picture" allow="autoplay; fullscreen" allowfullscreen></iframe></div><div class="message" id="message">Select your radio and press Start, then click a signal above.</div></section>

<aside class="panel readout"><h2>Receiver</h2><div class="datum"><label>Satellite frequency</label><strong id="sat">—</strong><small>MHz</small></div><div class="datum"><label>Receiver IF</label><strong id="if">—</strong><small>MHz</small></div><div class="datum"><label>Signal-to-noise</label><strong id="snr">—</strong><small>dB</small></div><div class="note">Picture uses the desktop player's decoded frames.<br>Keep this page open when tuning.<br>Sound stays in the desktop app in this prototype.</div></aside></div>

</main><script>

'use strict';

const controlToken='__ONAIR_CONTROL__';

const key=new URLSearchParams(location.search).get('key');

function endpoint(path){const url=new URL(path,location.origin);if(key)url.searchParams.set('key',key);return url;}

const canvas=document.getElementById('spectrum'),ctx=canvas.getContext('2d');

let state=null,hover=null,busy=false,initial=true,videoPort=null;

const low=10490.5,high=10499.5,rates=[25,33,50,66,125,250,333,500,1000,1500,2000];

const frequency=document.getElementById('frequency'),symbolrate=document.getElementById('symbolrate');

function message(text,error=false){const el=document.getElementById('message');el.textContent=text;el.classList.toggle('error',error);}

function estimate(mhz){const data=state?.spectrum;if(!data||data.length<8)return {mhz,sr:0};const n=data.length,step=(high-low)/(n-1);

const sm=data.map((_,i)=>{let sum=0,count=0;for(let k=-3;k<=3;k++){let j=i+k;if(j>=0&&j<n){sum+=data[j];count++;}}return sum/count;});

const noise=[...sm].sort((a,b)=>a-b)[Math.floor(n/2)],bin=Math.round((mhz-low)/step),radius=Math.round(.15/step);

let peak=Math.max(0,bin-radius);for(let i=peak;i<=Math.min(n-1,bin+radius);i++)if(sm[i]>sm[peak])peak=i;

if(sm[peak]-noise<2000)return {mhz,sr:0};const half=noise+(sm[peak]-noise)*.5;let l=peak,r=peak;while(l>0&&sm[l-1]>half)l--;while(r<n-1&&sm[r+1]>half)r++;

const centre=low+(l+r)*.5*step;if(Math.abs(centre-10491.5)<.3)return {mhz:10491.5,sr:1500};const measured=(r-l+1)*step*1000;

return {mhz:centre,sr:rates.reduce((best,v)=>Math.abs(Math.log(v/measured))<Math.abs(Math.log(best/measured))?v:best,rates[0])};}

function draw(){const rect=canvas.getBoundingClientRect(),ratio=window.devicePixelRatio||1,w=rect.width,h=rect.height;

if(canvas.width!==Math.round(w*ratio)||canvas.height!==Math.round(h*ratio)){canvas.width=Math.round(w*ratio);canvas.height=Math.round(h*ratio);}ctx.setTransform(ratio,0,0,ratio,0,0);ctx.clearRect(0,0,w,h);

const left=14,right=w-14,top=12,bottom=h-36,x=f=>left+(f-low)/(high-low)*(right-left);

ctx.strokeStyle='#263344';ctx.lineWidth=1;ctx.font='11px ui-monospace,monospace';ctx.fillStyle='#8193aa';ctx.textAlign='center';

for(let f=10491;f<=10499;f+=(w<600?2:1)){ctx.beginPath();ctx.moveTo(x(f),top);ctx.lineTo(x(f),bottom);ctx.stroke();ctx.fillText(f.toFixed(0),x(f),h-13);}for(let j=0;j<4;j++){const y=top+j*(bottom-top)/3;ctx.beginPath();ctx.moveTo(left,y);ctx.lineTo(right,y);ctx.stroke();}

const data=state?.spectrum;if(data?.length){ctx.beginPath();data.forEach((value,i)=>{const xx=left+i/(data.length-1)*(right-left),v=Math.max(0,Math.min(1,(value/65535-.12)/(.58-.12))),yy=bottom-v*(bottom-top);if(i===0)ctx.moveTo(xx,yy);else ctx.lineTo(xx,yy);});ctx.strokeStyle='#82d7b3';ctx.lineWidth=1.5;ctx.stroke();ctx.lineTo(right,bottom);ctx.lineTo(left,bottom);ctx.closePath();const g=ctx.createLinearGradient(0,top,0,bottom);g.addColorStop(0,'#82d7b330');g.addColorStop(1,'#82d7b302');ctx.fillStyle=g;ctx.fill();}

else{ctx.fillStyle='#8193aa';ctx.fillText('Waiting for the BATC spectrum',w/2,h/2);}

function marker(f,color){if(f<low||f>high)return;ctx.strokeStyle=color;ctx.beginPath();ctx.moveTo(x(f),top);ctx.lineTo(x(f),bottom);ctx.stroke();}

if(state)marker(state.satelliteMhz,'#6baff0');if(hover){marker(hover.mhz,'#f2dd91');ctx.fillStyle='#f2dd91';ctx.textAlign='left';ctx.fillText(hover.mhz.toFixed(3)+' MHz'+(hover.sr?' · '+hover.sr+' kS/s':''),Math.max(left,Math.min(right-240,x(hover.mhz)+8)),top+15);}}

function cursor(event){const r=canvas.getBoundingClientRect();return Math.max(low,Math.min(high,low+(event.clientX-r.left-14)/(r.width-28)*(high-low)));}

canvas.addEventListener('pointermove',event=>{hover=estimate(cursor(event));draw();});canvas.addEventListener('pointerleave',()=>{hover=null;draw();});

canvas.addEventListener('click',event=>{const target=estimate(cursor(event));frequency.value=target.mhz.toFixed(3);if(target.sr)symbolrate.value=target.sr;tune();});

document.getElementById('tuning').addEventListener('submit',event=>{event.preventDefault();tune();});

async function tune(){if(busy)return;const mhz=Number(frequency.value),sr=Number(symbolrate.value);if(!Number.isFinite(mhz)||mhz<low||mhz>high||!Number.isFinite(sr)||sr<0||sr>10000){message('Enter a frequency in the QO-100 wideband range and a valid symbol rate.',true);return;}

busy=true;const button=document.getElementById('tune');button.disabled=true;message('Tuning to '+mhz.toFixed(3)+' MHz…');try{const url=endpoint('/api/tune');url.searchParams.set('frequency',mhz);url.searchParams.set('symbolRate',sr);const response=await fetch(url,{method:'POST',headers:{'X-OnAir-Control':controlToken}});if(!response.ok)throw Error(await response.text());message('Tuning to '+mhz.toFixed(3)+' MHz · waiting for a picture');}catch(error){message(error.message,true);}finally{busy=false;button.disabled=false;}}

let radioBusy=false,radioSignature='';
const radioSelect=document.getElementById('radio');
async function radioCommand(action){if(radioBusy)return;radioBusy=true;updateRadios();try{const url=endpoint('/api/radio/'+action);if(action==='start')url.searchParams.set('device',radioSelect.value);const response=await fetch(url,{method:'POST',headers:{'X-OnAir-Control':controlToken}});if(!response.ok)throw Error(await response.text());message(action==='start'?'Starting receiver…':action==='stop'?'Stopping receiver…':'Looking for radios…');}catch(error){message(error.message,true);}finally{radioBusy=false;updateRadios();}}
for(const action of ['start','stop','refresh'])document.getElementById('radio-'+action).onclick=()=>radioCommand(action);
function updateRadios(){if(!state)return;const radios=state.radios||[],signature=JSON.stringify(radios);if(signature!==radioSignature){const previous=radioSelect.value;radioSelect.replaceChildren();for(const radio of radios){const option=document.createElement('option');option.value=radio.id;option.textContent=radio.name;radioSelect.append(option);}if(radios.some(r=>String(r.id)===previous))radioSelect.value=previous;else if(radios.some(r=>r.id===state.selectedRadio))radioSelect.value=state.selectedRadio;radioSignature=signature;}if(state.receiving)radioSelect.value=state.selectedRadio;radioSelect.disabled=radioBusy||state.receiving;document.getElementById('radio-start').disabled=radioBusy||state.receiving||!radios.length;document.getElementById('radio-stop').disabled=radioBusy||!state.receiving;document.getElementById('radio-refresh').disabled=radioBusy||state.receiving;document.getElementById('radio-status').textContent=radios.length?(state.receiving?'Receiving':'Ready'):'No radio found · connect one and press Refresh radios';}
async function poll(){try{const response=await fetch(endpoint('/api/receiver'),{cache:'no-store'});if(!response.ok)throw Error('Receiver unavailable');state=await response.json();updateRadios();video(state.videoPort);if(state.locked&&state.playerStatus==='playing'&&!busy)message('Live picture · '+(state.service||'received channel'));const badge=document.getElementById('connection');badge.textContent=state.locked?'LOCKED':state.receiving?'RECEIVING':'STOPPED';badge.classList.toggle('lock',state.locked);document.getElementById('sat').textContent=state.satelliteMhz.toFixed(3);document.getElementById('if').textContent=state.ifMhz.toFixed(3);document.getElementById('snr').textContent=state.locked?state.snrDb.toFixed(1):'—';document.getElementById('channel').textContent=state.service||state.playerStatus||'Waiting for reception';document.getElementById('batc').textContent=state.spectrum.length?'LIVE · 10490.5–10499.5 MHz':'BATC unavailable · reconnecting';if(initial){frequency.value=state.satelliteMhz.toFixed(3);initial=false;}if(!state.receiving&&!busy)message('Select your radio and press Start, then click a signal above.');draw();}catch(error){document.getElementById('connection').textContent='DISCONNECTED';message('Cannot reach OnAir. Keep the desktop app running.',true);}finally{setTimeout(poll,250);}}

function video(port){if(videoPort===port)return;videoPort=port;const url=new URL(location.href);url.port=String(port);url.pathname='/onair';url.search='controls=false&muted=true&autoplay=true&playsInline=true';document.getElementById('player').src=url;}

new ResizeObserver(draw).observe(canvas);poll();

</script></body></html>)ONAIR";

}

