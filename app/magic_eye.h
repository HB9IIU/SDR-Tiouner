#pragma once
inline constexpr const char* magicEyeScript = R"MAGICEYE(// Vintage phosphor eye, shared by Receiver and Auto Monitor. No network polling.
(()=>{
const rates=['1/4','1/3','2/5','1/2','3/5','2/3','3/4','4/5','5/6','8/9','9/10'];
// Approximate normal-frame DVB-S2 Es/N0 references, matching core dvbs_s2.cpp table 13.
// These guide the visual scale; they are not a measurement of decoder margin.
const references={QPSK:[-2.35,-1.24,-.30,1,2.23,3.10,4.03,4.68,5.18,6.20,6.42],
'8PSK':[null,null,null,null,5.50,6.62,7.91,null,9.35,10.69,10.98],
'16APSK':[null,null,null,null,null,8.97,10.21,11.03,11.61,12.89,13.13],
'32APSK':[null,null,null,null,null,null,12.73,13.64,14.28,15.69,16.05]};
const clamp=x=>Math.max(0,Math.min(1,x)),eyes=new Set();let animation=0,lastFrame=0;
function tick(now){animation=0;if(now-lastFrame<33){animation=requestAnimationFrame(tick);return;}const dt=Math.min(.1,(now-lastFrame)/1000||.033);lastFrame=now;let moving=false;
for(const eye of eyes){if(!eye.node.isConnected){eyes.delete(eye);continue;}const alpha=1-Math.exp(-dt/(eye.target<eye.value?.10:.22));eye.value+=(eye.target-eye.value)*alpha;eye.light+=(eye.bright-eye.light)*alpha;if(Math.abs(eye.value-eye.target)>.002||Math.abs(eye.light-eye.bright)>.002)moving=true;else{eye.value=eye.target;eye.light=eye.bright;}eye.draw();}if(moving)animation=requestAnimationFrame(tick);}
function wake(){if(!animation){lastFrame=performance.now()-33;animation=requestAnimationFrame(tick);}}
class MagicEye{
constructor(parent){this.node=document.createElement('div');this.node.style.cssText='display:flex;align-items:center;justify-content:center;gap:8px;flex-shrink:0;min-width:0;min-height:72px';this.canvas=document.createElement('canvas');this.canvas.style.cssText='width:72px;height:72px;display:block';this.canvas.setAttribute('role','img');this.caption=document.createElement('span');this.caption.style.cssText='font-size:12px;color:#a9bbd3;white-space:pre-line;font-variant-numeric:tabular-nums;display:block;flex:0 0 52px;width:52px;line-height:18px';this.node.append(this.canvas,this.caption);parent.append(this.node);this.value=this.target=this.light=this.bright=0;eyes.add(this);this.update(null);}
update(ch,running=true){this.updated=performance.now();const valid=!!(running&&ch?.enabled&&ch.locked&&!ch.error&&typeof ch.mer==='number'&&Number.isFinite(ch.mer));let reference=ch?.standard===2?references[ch.modulation]?.[rates.indexOf(ch.fec)]:null;
// Optional browser preference: scale overrides per DVB standard/modulation/FEC.
let goodMargin=5;try{const options=JSON.parse(localStorage.getItem('magicEyeScale')||'{}');if(Number.isFinite(options.goodMarginDb)&&options.goodMarginDb>0)goodMargin=options.goodMarginDb;const custom=options.thresholds?.[`${ch?.standard}:${ch?.modulation}:${ch?.fec}`];if(Number.isFinite(custom))reference=custom;}catch{}
const known=Number.isFinite(reference),fraction=valid?clamp(known?(ch.mer-reference)/goodMargin:ch.mer/15):0;
this.target=valid?.12+.88*fraction:0;this.bright=valid?1:0;
this.caption.textContent=valid?'MER\n'+ch.mer.toFixed(1)+' dB':(ch?.locked?'MER\nunavailable':'No lock');
this.node.title=valid?(known?'MER quality indication using an approximate modulation/FEC reference; not a measured decoding margin.':'MER quality indication: fallback scale 0–15 dB; decoding threshold unknown.'):'No valid locked MER measurement; RF strength is not used.';
this.canvas.setAttribute('aria-label',valid?'Signal quality, MER '+ch.mer.toFixed(1)+' dB':'Signal quality unavailable');wake();}
draw(){const c=this.canvas,dpr=Math.min(2,window.devicePixelRatio||1),size=Math.round(72*dpr);if(c.width!==size){c.width=c.height=size;}const g=c.getContext('2d');g.setTransform(dpr,0,0,dpr,0,0);g.clearRect(0,0,72,72);const x=36,y=36,r=29,light=this.light;
let glass=g.createRadialGradient(28,24,1,x,y,34);glass.addColorStop(0,'#152c22');glass.addColorStop(.65,'#06100c');glass.addColorStop(1,'#010403');g.fillStyle=glass;g.beginPath();g.arc(x,y,33,0,Math.PI*2);g.fill();
g.strokeStyle=`rgba(92,239,157,${.12+.3*light})`;g.lineWidth=1;g.shadowColor='#20ff83';g.shadowBlur=9*light;g.beginPath();g.arc(x,y,r+2,0,Math.PI*2);g.stroke();
g.save();g.beginPath();g.arc(x,y,r,0,Math.PI*2);g.arc(x,y,12,0,Math.PI*2,true);g.clip('evenodd');
const phosphor=g.createRadialGradient(x,y,10,x,y,r);phosphor.addColorStop(0,`rgba(100,255,165,${.04+.92*light})`);phosphor.addColorStop(.55,`rgba(32,240,112,${.03+.92*light})`);phosphor.addColorStop(1,`rgba(0,130,58,${.04+.65*light})`);g.fillStyle=phosphor;g.fillRect(0,0,72,72);
const shadow=(1-this.value)*Math.PI/2;g.shadowBlur=0;g.fillStyle='#020906';for(const a of [0,Math.PI]){g.beginPath();g.moveTo(x,y);g.arc(x,y,r+1,a-shadow,a+shadow);g.closePath();g.fill();}g.restore();g.shadowBlur=0;
const centre=g.createRadialGradient(32,31,1,x,y,13);centre.addColorStop(0,'#17251e');centre.addColorStop(.75,'#080e0a');centre.addColorStop(1,'#000');g.fillStyle=centre;g.beginPath();g.arc(x,y,12,0,Math.PI*2);g.fill();
g.strokeStyle='rgba(224,255,239,.13)';g.lineWidth=2;g.beginPath();g.arc(35,35,30,3.55,4.65);g.stroke();}
}
setInterval(()=>{for(const eye of eyes){if(!eye.node.isConnected){eyes.delete(eye);continue;}if(performance.now()-eye.updated>1800&&eye.bright){eye.update(null);}}},1000);
window.MagicEye=MagicEye;
})();)MAGICEYE";
