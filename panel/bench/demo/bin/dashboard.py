#!/usr/bin/env python3
"""Build the LAB-43 live-measurement dashboard from data/*.json.
Usage: python3 bin/dashboard.py [output.html]   (run from the demo/ folder or repo root)
Self-contained HTML (Plotly via CDN). No hardware needed — reads cached/just-measured JSON."""
import json, glob, os, sys, datetime

HERE = os.path.dirname(os.path.abspath(__file__))
DEMO = os.path.dirname(HERE)
DATA = os.path.join(DEMO, "data")

sweep = sorted([json.load(open(f)) for f in glob.glob(os.path.join(DATA, "duty_*.json"))],
               key=lambda d: d["duty"])
latjit = json.load(open(os.path.join(DATA, "lat_jit.json")))
internal = json.load(open(os.path.join(DATA, "internal_jitter.json")))
ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(DEMO, "dashboard.html")

HEAD = """<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>G6 LED Panel — live trigger characterization</title>
<script src="https://cdn.plot.ly/plotly-2.35.2.min.js"></script>
<style>
 :root{--ink:#101826;--mut:#5a6472;--ok:#177245;--bad:#b3261e;--accent:#2563eb}
 *{box-sizing:border-box}
 body{font:15px/1.5 -apple-system,Segoe UI,Roboto,sans-serif;margin:0;background:#0f1420;color:var(--ink)}
 .hero{background:linear-gradient(120deg,#1e293b,#0f172a);color:#fff;padding:22px 26px}
 .hero h1{margin:0;font-size:23px;letter-spacing:.2px}
 .hero .tag{color:#9fb3d1;font-size:13.5px;margin-top:5px}
 .hero .by{display:inline-block;margin-top:9px;background:#2563eb22;border:1px solid #2563eb66;color:#bcd0ff;
            padding:3px 10px;border-radius:20px;font-size:12.5px}
 .wrap{max-width:1120px;margin:0 auto;padding:16px}
 .cards{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin:6px 0 14px}
 .card{background:#fff;border-radius:12px;padding:14px 16px;box-shadow:0 2px 10px rgba(0,0,0,.25)}
 .card .k{color:var(--mut);font-size:12px;text-transform:uppercase;letter-spacing:.5px}
 .card .v{font-size:27px;font-weight:700;margin-top:3px}
 .card .v small{font-size:14px;font-weight:600;color:var(--mut)}
 .card .s{color:var(--mut);font-size:12px;margin-top:2px}
 .panel{background:#fff;border-radius:12px;padding:14px 18px;margin:12px 0;box-shadow:0 2px 10px rgba(0,0,0,.22)}
 .panel h2{font-size:16px;margin:.1em 0 .5em}
 .panel .h-sub{color:var(--mut);font-size:13px;margin:-.3em 0 .6em}
 .grid2{display:grid;grid-template-columns:1.6fr 1fr;gap:14px}
 .ctl{display:flex;gap:24px;flex-wrap:wrap;align-items:center;margin-bottom:8px}
 .ctl label{font-weight:600}
 input[type=number]{width:74px;padding:5px 7px;font-size:15px;border:1px solid #c6ccd2;border-radius:6px}
 .summary{font-size:15px;margin:8px 0 4px;padding:8px 12px;background:#eef4ff;border-radius:8px}
 .rec{font-weight:700;color:var(--ok)}
 table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums;font-size:13.5px}
 th,td{padding:6px 9px;text-align:right;border-bottom:1px solid #eef1f4}
 th{background:#f0f2f5} td:first-child,th:first-child{text-align:left}
 tr.fit{background:#e9f8ec} tr.nofit{background:#fbedee;color:#9aa0a6}
 .yes{color:var(--ok);font-weight:700}.no{color:var(--bad);font-weight:700}
 .swatch{display:inline-block;width:11px;height:11px;border-radius:2px;margin-right:6px;vertical-align:middle}
 .note{color:var(--mut);font-size:12.5px}
 @media(max-width:820px){.cards{grid-template-columns:repeat(2,1fr)}.grid2{grid-template-columns:1fr}}
</style></head><body>
<div class="hero">
 <h1>G6 LED Panel — live trigger characterization</h1>
 <div class="tag">v0.3.1 two-PIO firmware · external trigger → photons, measured end-to-end</div>
 <div class="by">⚡ measured live by Claude — function generator + SPI controller + photodiode · __TS__</div>
</div>
<div class="wrap">
"""

CARDS = """
<div class="cards">
 <div class="card"><div class="k">trigger → LED latency</div><div class="v">__LAT__ <small>µs</small></div>
   <div class="s">photodiode, time-of-flight from the edge</div></div>
 <div class="card"><div class="k">timing jitter (external)</div><div class="v">__JITNS__ <small>ns RMS</small></div>
   <div class="s">photodiode onset spread, n=__N__</div></div>
 <div class="card"><div class="k">scan jitter (internal)</div><div class="v">__DWT__ <small>µs</small></div>
   <div class="s">panel's own DWT cycle-counter</div></div>
 <div class="card"><div class="k">brightness levels swept</div><div class="v">__LV__</div>
   <div class="s">on-time 2.7 → 45 µs (live)</div></div>
</div>
"""

BODY = """
<div class="panel">
 <h2>1 · Trigger → LED response &nbsp;<span class="note">(external ground truth — the actual photons)</span></h2>
 <div class="grid2">
   <div><div id="latPlot" style="height:300px"></div></div>
   <div><div id="jitPlot" style="height:300px"></div></div>
 </div>
 <div class="note">Left: averaged photodiode response vs time after the external trigger (t=0). The LED turns on
   <b>__LAT__ µs</b> later. Right: distribution of that onset across __N__ single triggers — width = <b>__JITNS__ ns RMS</b> jitter.
   For contrast, the panel's <i>own</i> DWT cycle-counter reports a full-scan-traversal jitter of __DWT__ µs (a different,
   software-loop axis) — Claude read both the photons and the chip's internal timing register.</div>
</div>

<div class="panel">
 <h2>2 · Brightness vs sync window &nbsp;<span class="note">(set your timing budget — which levels fit?)</span></h2>
 <div class="ctl">
   <span><label>Time budget</label> <input id="budget" type="number" value="50" min="1" step="1"> µs</span>
   <span><label>Trigger rate</label> <input id="freq" type="number" value="8" min="0.1" step="0.5"> kHz → interval <b id="interval">125.0</b> µs</span>
   <span><label>Fit metric</label>
     <label class="note"><input type="radio" name="metric" value="ontime" checked> on-time</label>
     <label class="note"><input type="radio" name="metric" value="offby"> trigger→LED-off</label>
   </span>
   <span><button id="snd" type="button" style="padding:5px 10px;border-radius:6px;border:1px solid #c6ccd2;background:#fff;cursor:pointer">🔇 sound</button></span>
 </div>
 <div class="summary" id="summary"></div>
 <div class="grid2">
   <div><div id="sweepPlot" style="height:380px"></div></div>
   <div style="max-height:380px;overflow:auto"><table id="tbl"><thead><tr>
     <th>duty</th><th>on-time µs</th><th>off-by µs</th><th>% int</th><th>fits</th></tr></thead><tbody></tbody></table></div>
 </div>
 <div class="note">Each trace = measured photodiode waveform at that <code>duty_cycle</code>. Green dotted line = your deadline;
   fitting levels are bold. Pick the brightest level (highest duty) that still fits your window.</div>
</div>

<div class="panel note">
 <b>How this was measured:</b> Claude drove an AD3 function generator (W1, 8 kHz external trigger → panel EINT),
 commanded the Teensy arena to stream V1 Triggered frames over SPI, and captured the panel's emitted light on an
 AD3 photodiode (Ch1, 12.5 MS/s, trigger-averaged). The internal scan-jitter is read from the panel's DWT cycle
 counter over USB. v0.3.1 firmware: rows on PIO1 + columns on PIO0, DMA-fed (two-PIO scanner). Branch
 <code>mreiser/lab-43-v031-twopio</code>.
</div>
</div>

<script>
const SWEEP=__SWEEP__, LATJIT=__LATJIT__, INTERNAL=__INTERNAL__;
const N=SWEEP.length, onsets=SWEEP.map(d=>d.onset).sort((a,b)=>a-b), medOnset=onsets[Math.floor(N/2)];
function color(i){return `hsl(${240-240*i/(N-1)},72%,46%)`;}

// --- Plot 1a: latency waveform ---
Plotly.newPlot('latPlot',[
  {x:LATJIT.t,y:LATJIT.v,mode:'lines',line:{color:'#2563eb',width:2.5},name:'photodiode'}
],{margin:{t:8,r:10,b:42,l:50},xaxis:{title:'µs after trigger',range:[-2,8],zeroline:false},
   yaxis:{title:'photodiode (mV)'},showlegend:false,
   shapes:[{type:'line',x0:0,x1:0,yref:'paper',y0:0,y1:1,line:{color:'#111',dash:'dash',width:1.4}},
           {type:'line',x0:LATJIT.latency_us,x1:LATJIT.latency_us,yref:'paper',y0:0,y1:1,line:{color:'#b3261e',dash:'dot',width:2}}],
   annotations:[{x:0,y:1,yref:'paper',yanchor:'bottom',text:'trigger',showarrow:false,font:{size:11}},
                {x:LATJIT.latency_us,y:0.5,yref:'paper',text:`+${LATJIT.latency_us} µs`,showarrow:false,xanchor:'left',font:{size:12,color:'#b3261e'}}]},
   {displayModeBar:false,responsive:true});

// --- Plot 1b: onset jitter histogram ---
Plotly.newPlot('jitPlot',[
  {x:LATJIT.onsets_us,type:'histogram',marker:{color:'#2563eb'},xbins:{size:0.04}}
],{margin:{t:8,r:10,b:42,l:50},xaxis:{title:'trigger→LED onset (µs)'},yaxis:{title:'count'},
   shapes:[{type:'line',x0:LATJIT.latency_us,x1:LATJIT.latency_us,yref:'paper',y0:0,y1:1,line:{color:'#b3261e',dash:'dot',width:2}}],
   annotations:[{x:LATJIT.latency_us,y:1,yref:'paper',yanchor:'bottom',text:`σ=${(LATJIT.jitter_us*1000).toFixed(0)} ns`,showarrow:false,font:{size:12,color:'#b3261e'}}]},
   {displayModeBar:false,responsive:true});

// --- Plot 2: duty/on-time explorer ---
function buildSweep(){
  const traces=SWEEP.map((d,i)=>({x:d.t,y:d.v,mode:'lines',name:`duty ${d.duty} — ${d.fwhm}µs`,
     line:{color:color(i),width:2},hovertemplate:`duty ${d.duty}: %{y:.0f} mV @ %{x:.1f} µs<extra></extra>`}));
  Plotly.newPlot('sweepPlot',traces,{margin:{t:8,r:10,b:42,l:50},
     xaxis:{title:'µs after trigger',range:[-4,50],zeroline:false},yaxis:{title:'photodiode (mV)'},
     hovermode:'closest',legend:{orientation:'h',y:-0.2,font:{size:10}},
     shapes:[{type:'line',x0:0,x1:0,yref:'paper',y0:0,y1:1,line:{color:'#111',dash:'dash',width:1.3}},
             {type:'line',x0:20,x1:20,yref:'paper',y0:0,y1:1,line:{color:'#177245',dash:'dot',width:2}}]},
     {displayModeBar:false,responsive:true});
}
const $=id=>document.getElementById(id);
function metric(){return document.querySelector('input[name=metric]:checked').value;}
function fitVal(d,m){return m==='ontime'?d.fwhm:d.onset+d.fwhm;}
// --- Web Audio: reveal chime on enable + a soft tone as a level crosses the budget ---
let AC=null,sndOn=false,prevFit=null;
function tone(f,dur,vol,type){if(!AC||!sndOn)return;const o=AC.createOscillator(),g=AC.createGain();
  o.type=type||'sine';o.frequency.value=f;o.connect(g);g.connect(AC.destination);const t=AC.currentTime;
  g.gain.setValueAtTime(0.0001,t);g.gain.exponentialRampToValueAtTime(vol||0.12,t+0.012);
  g.gain.exponentialRampToValueAtTime(0.0001,t+(dur||0.2));o.start(t);o.stop(t+(dur||0.2)+0.02);}
function dutyFreq(d){return 320+(d/255)*880;}
function reveal(){[523.25,659.25,783.99,1046.5].forEach((f,i)=>setTimeout(()=>tone(f,0.22,0.13,'triangle'),i*95));}
$('snd').addEventListener('click',()=>{if(!AC)AC=new (window.AudioContext||window.webkitAudioContext)();
  sndOn=!sndOn;$('snd').textContent=sndOn?'🔊 sound on':'🔇 sound';if(sndOn){AC.resume();reveal();}});
function update(){
  const budget=+$('budget').value,freq=+$('freq').value,m=metric(),interval=1000/freq;
  $('interval').textContent=interval.toFixed(1);
  const deadlineX=(m==='ontime'?medOnset:0)+budget;
  const fitArr=SWEEP.map(d=>fitVal(d,m)<=budget);
  Plotly.restyle('sweepPlot',{opacity:fitArr.map(f=>f?1:0.18),'line.width':fitArr.map(f=>f?3:1.2)});
  Plotly.relayout('sweepPlot',{'shapes[1].x0':deadlineX,'shapes[1].x1':deadlineX});
  if(sndOn&&prevFit){SWEEP.forEach((d,i)=>{if(fitArr[i]!==prevFit[i]){
    const f=dutyFreq(d.duty);tone(fitArr[i]?f:f*0.6,0.16,0.11,fitArr[i]?'triangle':'sine');}});}
  prevFit=fitArr;
  let nfit=0,maxD=null,rows='';
  SWEEP.forEach((d,i)=>{const off=d.onset+d.fwhm,fits=fitArr[i];if(fits){nfit++;maxD=d.duty;}
    rows+=`<tr class="${fits?'fit':'nofit'}"><td><span class="swatch" style="background:${color(i)}"></span>${d.duty}</td>`+
          `<td>${d.fwhm.toFixed(1)}</td><td>${off.toFixed(1)}</td><td>${(d.fwhm/interval*100).toFixed(0)}%</td>`+
          `<td class="${fits?'yes':'no'}">${fits?'✓':'✗'}</td></tr>`;});
  $('tbl').querySelector('tbody').innerHTML=rows;
  $('summary').innerHTML=`<b>${nfit}/${N}</b> levels fit a <b>${budget} µs</b> budget`+
     (maxD!==null?` → brightest that fits: <span class="rec">duty_cycle ${maxD}</span>.`:` → none; raise budget.`)+
     `  (${freq} kHz ⇒ ${interval.toFixed(1)} µs interval)`;
}
buildSweep();
['budget','freq'].forEach(id=>$(id).addEventListener('input',update));
document.querySelectorAll('input[name=metric]').forEach(e=>e.addEventListener('change',update));
update();
</script></body></html>
"""

dwt = next(p for p in internal["panels"] if "two-PIO" in p["rev"])["jitter_us"]
html = (HEAD.replace("__TS__", ts)
        + CARDS.replace("__LAT__", f'{latjit["latency_us"]:.2f}')
               .replace("__JITNS__", f'{latjit["jitter_us"]*1000:.0f}')
               .replace("__N__", str(latjit["n"]))
               .replace("__DWT__", f'{dwt:.1f}')
               .replace("__LV__", str(len(sweep)))
        + BODY.replace("__LAT__", f'{latjit["latency_us"]:.2f}')
              .replace("__JITNS__", f'{latjit["jitter_us"]*1000:.0f}')
              .replace("__N__", str(latjit["n"]))
              .replace("__DWT__", f'{dwt:.1f}')
              .replace("__SWEEP__", json.dumps(sweep))
              .replace("__LATJIT__", json.dumps(latjit))
              .replace("__INTERNAL__", json.dumps(internal)))
open(out, "w").write(html)
print(f"wrote {out} ({len(html)} bytes; {len(sweep)} levels, latency {latjit['latency_us']}us, jitter {latjit['jitter_us']*1000:.0f}ns)")
