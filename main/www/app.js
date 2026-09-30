"use strict";
var $=function(id){return document.getElementById(id)};
// Write html into el only when it actually changed (cached on el.__h). The status
// poll re-renders every 4 s; blindly reassigning innerHTML would destroy and recreate
// the child nodes, restarting any CSS animation on them from 0% — making looping
// animations (hero ring, "searching" signal bars, pulsing dot) visibly jump. Skipping
// the write when nothing changed keeps the same nodes alive so the animation runs on.
function setHTML(el,html){ if(el && el.__h!==html){ el.__h=html; el.innerHTML=html; } }
var state=null, otaTimer=null, otaAvail=null, waking=false, wakeTimeout=null, chgBusy=false, feedOk=false;
var otaBusy=false, otaPhase=null, otaPollState=null, otaDeadline=0, otaExpectedVersion=null, otaChannel='release', otaChannelInitialSet=false;
var otaChannelInFlight=false, otaChannelTarget=null, otaPrompting=false;
var fwFocusId=null;   // control that started the running check/channel change; refocused once the UI is idle again

function isOtaRunning(){
  if(otaPhase === 'update' || otaPhase === 'reboot') return true;
  var st = (typeof otaPollState === 'string') ? otaPollState : (otaPollState && otaPollState.state);
  return st === 'downloading' || st === 'done';
}
function isOtaBusy(){
  return !!(otaBusy || isOtaRunning() || otaChannelInFlight || otaPhase === 'check');
}
// Entry points of the firmware flows (check, channel change, update prompt). otaPrompting spans "an
// update was found" until the install dialog is answered, changelog fetch included, so a second tap
// cannot start a second flow; the controls stay enabled (so a dismissed dialog can hand focus back to
// them) and the entry points simply ignore the tap. The other settings stay usable as well: an open
// sheet and the update dialog may coexist by design (see syncModal).
function isOtaFlowBusy(){ return isOtaBusy() || otaPrompting; }

// Quotes are escaped too so esc() is safe in attribute values (title="…"), not just element content.
function esc(s){return String(s).replace(/[&<>"']/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]})}
// Numeric fields from the /status JSON are coerced before they touch any HTML string —
// a finite number in, the number out; anything else null. Keeps radio-sourced JSON
// (RSSI, SOC, progress) from ever reaching innerHTML as text.
function num(x){ x=+x; return isFinite(x)?x:null; }

function requestJson(url,options){
  return fetch(url,options).then(function(r){
    if(!r||!r.ok){ var e=new Error('HTTP '+(r&&r.status!=null?r.status:'error')); e.status=r&&r.status; throw e; }
    return r.json();
  });
}
function requestJsonResult(url,options){
  return fetch(url,options).then(function(r){
    return r.json().catch(function(){ return null; }).then(function(j){
      return {ok:!!(r&&r.ok), status:r?r.status:0, json:j};
    });
  });
}
// send is requestJson or requestJsonResult; either way a request that never settles is aborted
// and rejected after timeoutMs, so a hung device cannot leave the UI waiting forever.
function timedRequest(send,url,options,timeoutMs){
  var ctl=typeof AbortController!=='undefined'?new AbortController():null;
  var opts=Object.assign({},options||{}); if(ctl)opts.signal=ctl.signal;
  return new Promise(function(resolve,reject){
    var settled=false;
    var timer=setTimeout(function(){
      if(settled)return; settled=true; if(ctl)ctl.abort(); reject(new Error('request timed out'));
    },timeoutMs);
    send(url,opts).then(function(value){
      if(settled)return; settled=true; clearTimeout(timer); resolve(value);
    },function(error){
      if(settled)return; settled=true; clearTimeout(timer); reject(error);
    });
  });
}
function requestJsonWithTimeout(url,options,timeoutMs){ return timedRequest(requestJson,url,options,timeoutMs); }
function requestJsonResultWithTimeout(url,options,timeoutMs){ return timedRequest(requestJsonResult,url,options,timeoutMs); }
function commandResponse(j){
  var o=j&&j.response;
  if(!o||typeof o.result!=='boolean'||typeof o.reason!=='string') throw new Error('invalid command response');
  return o;
}

/* ---------- toasts ---------- */
function toast(msg, type, key){
  type = type || 'info';
  var c = $("toasts");
  if(!c) return null;

  var t = null;
  // 1. If key is provided, find any existing toast element with this key
  if(key){
    for(var i = 0; i < c.children.length; i++){
      var item = c.children[i];
      if(item.dataset && item.dataset.key === key){
        t = item;
        break;
      }
    }
  }

  // 2. If not found by key, try to reuse an existing non-load toast
  if(!t){
    for(var j = 0; j < c.children.length; j++){
      var el = c.children[j];
      var isLoad = el.className ? (/\bload\b/.test(el.className)) : (el.classList && el.classList.contains('load'));
      if(!isLoad){
        t = el;
        break;
      }
    }
  }

  // 3. Otherwise create a new toast element so active load toasts are not destroyed
  if(!t){
    t = document.createElement('div');
    c.appendChild(t);
  } else {
    t.classList.remove('leaving');
  }

  if(t._toastTimer){ clearTimeout(t._toastTimer); t._toastTimer = null; }
  if(t._toastLeavingTimer){ clearTimeout(t._toastLeavingTimer); t._toastLeavingTimer = null; }

  t.className = 'toast ' + type;
  if(key) t.dataset.key = key;
  else if(t.dataset && t.dataset.key) delete t.dataset.key;

  var iconHtml = '';
  if(type === 'load'){
    iconHtml = '<svg class="otaspin" width="14" height="14" viewBox="0 0 16 16">' +
      '<circle cx="8" cy="8" r="6" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-dasharray="11 38"/>' +
      '</svg>';
  } else if(type === 'ok'){
    iconHtml = '✓';
  } else if(type === 'err'){
    iconHtml = '!';
  } else {
    iconHtml = 'i';
  }

  t.innerHTML = '<span class="ic">' + iconHtml + '</span><span>' + esc(msg) + '</span>';

  if(type !== 'load'){
    var dur = type === 'err' ? 5200 : 3000;
    t._toastTimer = setTimeout(function(){
      t._toastTimer = null;
      t.classList.add('leaving');
      t._toastLeavingTimer = setTimeout(function(){
        t._toastLeavingTimer = null;
        if(t.parentNode) t.parentNode.removeChild(t);
      }, 230);
    }, dur);
  }

  return t;
}

/* ---------- net ----------
   The live feed is an interval poll of GET /status (boot() sets it up), and poll() is also called
   directly after a user action so the UI refreshes now instead of on the next tick. A failed fetch
   deliberately changes nothing on screen — the last frame stays and the optimistic local state
   carries the UI — but it clears feedOk, which parks the Bluetooth countdown (see boot()).
   waitReboot() GETs /status too; that's post-OTA reboot detection, a different concern. */
function poll(){
  // Cache-bust + no-store: the live page polls forever, so a stale/cached /status would
  // freeze the hero on an old state (e.g. a transient orange "Unreachable") until a manual
  // reload — the very bug a fresh document hides. Same guard waitReboot() already uses.
  return requestJson('/status?ms='+Date.now(),{cache:'no-store'})
    .then(function(s){ feedOk=true; render(s); settleWakeToast(); })
    .catch(function(){ feedOk=false; });
}
// render() ends `waking` once the car reports data; the sticky "waiting for the car" toast is
// resolved here, in the poll path, so the renderer stays a pure state→DOM function.
function settleWakeToast(){
  if(waking) return;
  var c=$("toasts"); if(!c||!c.children) return;
  for(var i=0;i<c.children.length;i++){
    var el=c.children[i];
    if(el.dataset&&el.dataset.key==='wake'&&/\bload\b/.test(el.className||'')){ toast('Car is awake','ok','wake'); return; }
  }
}

/* ---------- signal glyph ---------- */
function barsHTML(rssi){
  // rssi==null → no link (e.g. BLE disconnected): draw the glyph with every bar faded so
  // the row still shows a signal placeholder instead of jumping to text-only.
  rssi=num(rssi);
  var n = rssi==null ? 0 : (rssi>=-55?4 : rssi>=-67?3 : rssi>=-78?2 : rssi>=-90?1:0);
  var H=[4,7,10,13], s='<svg class="bars" width="24" height="15" viewBox="0 0 21 13" fill="currentColor"><title>'+(rssi==null?'No signal':rssi+' dBm')+'</title>';
  for(var i=0;i<4;i++) s+='<rect x="'+(i*5.3)+'" y="'+(13-H[i])+'" width="3.3" height="'+H[i]+'" rx="1" opacity="'+(i<n?1:0.25)+'"/>';
  return s+'</svg>';
}
// animated "searching…" glyph — same geometry as barsHTML, but each bar carries an
// .sbN class whose @keyframes light it in turn (cumulative fill), looping until the link
// is up. Opacity is driven entirely by CSS, so no opacity attr here. `warm` switches the
// fill to the amber the UI already uses for "link up but nothing known yet", so hunting
// for the car reads as the same in-between state on the BLE row while Wi-Fi's own search
// stays green.
function searchBarsHTML(warm){
  var H=[4,7,10,13], s='<svg class="bars search'+(warm?' warm':'')+'" width="24" height="15" viewBox="0 0 21 13" fill="currentColor"><title>Searching…</title>';
  for(var i=0;i<4;i++) s+='<rect class="sb'+(i+1)+'" x="'+(i*5.3)+'" y="'+(13-H[i])+'" width="3.3" height="'+H[i]+'" rx="1"/>';
  return s+'</svg>';
}
// "no link" glyph — the same four bars drawn as empty OUTLINES (light stroke, pale fill)
// rather than faded solids, so a disconnected row reads as an unfilled gauge instead of a
// dimmed reading, and sits visibly lighter than the "Disconnected" label next to it.
function emptyBarsHTML(){
  // Same envelope as barsHTML — inset by half a stroke so the outlined bar occupies exactly
  // the footprint the filled one would, and the row doesn't visibly change size or weight
  // when the link comes up.
  var H=[4,7,10,13], s='<svg class="bars empty" width="24" height="15" viewBox="0 0 21 13"><title>No signal</title>';
  for(var i=0;i<4;i++) s+='<rect x="'+(i*5.3+0.45)+'" y="'+(13-H[i]+0.45)+'" width="2.4" height="'+(H[i]-0.9)+'" rx="0.9"/>';
  return s+'</svg>';
}
/* ---------- Bluetooth phase countdown ----------
   The device reports which BLE phase is running and how many seconds are left in it
   (ble.phase / ble.phase_s): "connecting" = an attempt is running and gives up then,
   "waiting" = no attempt is running and the next one starts then. A fresh value only
   arrives with each 4 s poll, so the remaining time is ticked down locally in between and the
   polled value is treated as the clock to resync against.

   The time sits at the end of the row's status line, and is painted with textContent into a dedicated node — NOT rebuilt through setHTML with
   the rest of the row, since rewriting the row's innerHTML every second would re-create the
   bar <rect>s and restart their CSS fill animation on every tick. */
var cdKind=null, cdEndMs=0;
// Constant markup — only the node's text changes per tick. data-p names the ONE countdown this
// row will render, so a row can never show a number belonging to a different one.
function cdHTML(kind){ return '<span class="cd" data-p="'+kind+'"></span>'; }
// Resync the local clock from a poll. A change of countdown always resyncs; within one, only a
// real disagreement (≥2 s) does, so the 4 s poll cadence and the 1 s local tick can't fight
// each other into a number that jitters back up.
function cdSync(kind,secs){
  if(kind!=='givesup'&&kind!=='retries'){ cdKind=null; cdEndMs=0; return; }
  if(secs==null) secs=0;
  if(kind!==cdKind||Math.abs(cdLeft()-secs)>=2){ cdKind=kind; cdEndMs=Date.now()+secs*1000; }
}
function cdLeft(){ return Math.max(0,Math.ceil((cdEndMs-Date.now())/1000)); }
// Paint into the row's .cd node if the current row has one. The node declares (data-p) the one
// countdown it renders and a mismatch paints nothing. At 0 the phase is over but the next push
// hasn't landed yet, so say what is about to happen rather than dropping the text and leaving
// the status line cut off for a beat.
function paintCd(){
  var el=document.querySelector('#bleConn .cd'); if(!el) return;
  var on=cdKind&&el.getAttribute('data-p')===cdKind;
  el.classList.toggle('off',!on);
  if(!on) return;
  var n=cdLeft();
  el.textContent = cdKind==='retries' ? (n?'retry in '+n+'s':'retrying…')
                                      : (n?n+'s left'      :'timing out…');
}

/* ---------- BLE row decision — PARITY-CHECKED, do not edit one side only ----------
   This is the JavaScript half of main/logic/ble_row.hpp. scripts/check-ble-row-parity.sh
   compiles the C++ presenter, dumps its decision for a sweep of inputs, and re-decides the
   same inputs here — CI fails if the two ever disagree, so the browser cannot silently drift
   from the host-tested rules. The region between the BLE_ROW markers is what that harness
   extracts and evaluates; keep it free of DOM access and browser globals.

   `ble.scanning` is deliberately absent. The radio scans for reasons that carry no deadline
   and no schedule, and driving the label off that flag while the countdown came from the phase
   is what made the two disagree for three rounds. Leaving it out makes that unrepresentable.  */
/* BLE_ROW_BEGIN */
var BLE_CONNECT_FAIL_WARN = 2;
// Takes the /status object as it arrives and returns the row decision. The DERIVATIONS live in
// here on purpose — "is there a VIN", "is the link known" — because an earlier cut computed them
// at the call site, outside the fence, which is the one step no gate could see. That is exactly
// where the `paired &&` factoring lives: drop it and an unpaired cold-start board renders the
// linked row amber instead of green, with every golden vector still passing.
function bleRowFromStatus(s){
  var ble = s.ble || {};
  var vin = s.vin;
  var hasVin = !!(vin && vin !== 'UNKNOWN');
  var linkKnown = !(!!s.paired && (s.link === 'unknown' || s.link === 'unreachable'));
  if(!hasVin) return { row:((ble.devices||[]).length > 0 ? 'listing' : 'discovering'), cd:'none', stateless:false };
  if(ble.connected) return { row:'linked', cd:'none', stateless:!linkKnown };
  if((+ble.connect_fail || 0) >= BLE_CONNECT_FAIL_WARN) return { row:'failed', cd:'none', stateless:false };
  if(ble.phase === 'connecting') return { row:'scanning', cd:'givesup', stateless:false };
  return { row:'idle', cd:(ble.phase === 'waiting' ? 'retries' : 'none'), stateless:false };
}
/* BLE_ROW_END */
// "status unknown" glyph — same geometry as barsHTML, all four bars fully lit (the link
// is up; signal strength is real and shown as dBm next to it). Each bar carries a .wbN
// class; one @keyframes wave with staggered delays ripples an amber wave left→right
// across the bars. The markup is rssi-independent so it stays
// byte-identical across polls — setHTML keeps the nodes, so the animation never restarts.
function waveBarsHTML(){
  var H=[4,7,10,13], s='<svg class="bars wave" width="24" height="15" viewBox="0 0 21 13"><title>Vehicle status unknown</title>';
  for(var i=0;i<4;i++) s+='<rect class="wb'+(i+1)+'" x="'+(i*5.3)+'" y="'+(13-H[i])+'" width="3.3" height="'+H[i]+'" rx="1"/>';
  return s+'</svg>';
}
// continuous SOC colour: red → amber → light green → signal-bar green, interpolated
// so the ring fades smoothly as it charges instead of jumping between bands. The top
// stop is --ok (#16a34a = rgb(22,163,74)), the same green as the Wi-Fi/BLE signal bars,
// so a full battery reads as a lighter, less-intense green rather than a dark forest green.
function socColor(p){
  p=Math.max(0,Math.min(100,p));
  var stops=[[0,232,33,39],[10,217,164,6],[50,101,196,102],[100,22,163,74]];
  var a=stops[0],b=stops[stops.length-1];
  for(var i=0;i<stops.length-1;i++){ if(p>=stops[i][0] && p<=stops[i+1][0]){ a=stops[i]; b=stops[i+1]; break; } }
  var t=(p-a[0])/((b[0]-a[0])||1);
  var ch=function(j){ return Math.round(a[j]+(b[j]-a[j])*t); };
  return 'rgb('+ch(1)+','+ch(2)+','+ch(3)+')';
}
/* ---------- icons (2px outline shapes; stroke styling lives in CSS) ---------- */
var ICON={
  bolt:'<use href="#ic-bolt"/>',
  moon:'<path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z"/>',
  car:'<use href="#ic-car"/>',
  alarm:'<circle cx="12" cy="13" r="8"/><path d="M12 9v4l2 2"/><path d="M5 3 2 6"/><path d="m22 6-3-3"/>',
  bluetooth:'<use href="#ic-bt"/>',
  pencil:'<use href="#ic-pencil"/>',
  key:'<use href="#ic-key"/>',
  link:'<path d="M10 13a5 5 0 0 0 7.5.5l3-3a5 5 0 0 0-7-7l-1.7 1.7"/><path d="M14 11a5 5 0 0 0-7.5-.5l-3 3a5 5 0 0 0 7 7l1.7-1.7"/>',
  search:'<circle cx="11" cy="11" r="7"/><path d="m20 20-4-4"/>'
};
function iconSVG(name){ return '<svg viewBox="0 0 24 24" aria-hidden="true">'+(ICON[name]||'')+'</svg>'; }

/* ---------- battery gauge ----------
   A fixed 240-unit box scaled by CSS (--gs). mode: soc | charging (flowing segment) | complete |
   sleep (muted last-known arc) | waking | checking | empty | neutral. Pure markup, so setHTML()
   keeps the live nodes and their running animation between polls. */
function gaugeHTML(o){
  var S=240, sw=14, c=S/2, r=c-sw/2-3, C=2*Math.PI*r;
  var pct=(o.pct==null)?null:Math.max(0,Math.min(100,o.pct)), mode=o.mode;
  var arc=(pct!=null)&&(mode==='soc'||mode==='charging'||mode==='complete'||mode==='sleep'||mode==='waking');
  var col=(mode==='sleep'||mode==='waking')?'var(--muted)':socColor(pct||0);
  var s='<svg viewBox="0 0 '+S+' '+S+'" aria-hidden="true" style="transform:rotate(-90deg)">'+
        '<circle cx="'+c+'" cy="'+c+'" r="'+r.toFixed(1)+'" fill="none" stroke="var(--soft)" stroke-width="'+sw+'"/>';
  if(arc){
    var pc=mode==='sleep'?' pulse slow':mode==='waking'?' pulse fast':'';
    s+='<circle class="garc'+pc+'" cx="'+c+'" cy="'+c+'" r="'+r.toFixed(1)+'" fill="none" stroke="'+col+'" stroke-width="'+sw+'" stroke-linecap="round"'+
       ' stroke-dasharray="'+C.toFixed(1)+'" stroke-dashoffset="'+(C*(1-pct/100)).toFixed(1)+'"/>';
    if(mode==='charging'&&pct>6){
      // Highlight pass: fades in at the arc start, glides to the arc end at a constant ~140 units/s
      // (the first 72 % of the cycle, see @keyframes flow) and fades out before it gets there; the
      // rest of the cycle is a pause, so the wrap back to 12 o'clock is never visible. The cycle
      // length follows the arc length so a short and a long arc glide at the same calm speed.
      var seg=sw*2.2, dist=Math.max(0,C*pct/100-seg), dur=Math.max(3,Math.min(8,dist/140/.72));
      s+='<circle class="gflow" cx="'+c+'" cy="'+c+'" r="'+r.toFixed(1)+'" fill="none" stroke="rgba(255,255,255,.75)" stroke-width="'+(sw-3)+'" stroke-linecap="round"'+
         ' stroke-dasharray="'+seg.toFixed(1)+' '+(C*2).toFixed(1)+'" style="--flow-end:'+(-dist).toFixed(1)+'px;--flow-dur:'+dur.toFixed(1)+'s"/>';
    }
  }
  if(o.limit!=null&&(mode==='soc'||mode==='charging'||mode==='complete')){
    var a=o.limit/100*2*Math.PI, r1=r-sw/2-3, r2=r+sw/2+3;
    s+='<line x1="'+(c+r1*Math.cos(a)).toFixed(1)+'" y1="'+(c+r1*Math.sin(a)).toFixed(1)+'" x2="'+(c+r2*Math.cos(a)).toFixed(1)+'" y2="'+(c+r2*Math.sin(a)).toFixed(1)+'" stroke="var(--fg)" stroke-width="2" stroke-linecap="round"/>';
  }
  s+='</svg>';
  if(mode==='waking'||mode==='checking'||o.busy){
    s+='<svg class="gspin'+(mode==='waking'?' fast':'')+'" viewBox="0 0 '+S+' '+S+'" aria-hidden="true"><circle cx="'+c+'" cy="'+c+'" r="'+r.toFixed(1)+'" fill="none"'+
       ' stroke="'+(o.busy?'var(--fg)':'var(--muted)')+'" stroke-opacity="'+(o.busy?'.55':'.8')+'" stroke-width="'+(sw*.5)+'" stroke-linecap="round" stroke-dasharray="'+(C*.14).toFixed(1)+' '+C.toFixed(1)+'"/></svg>';
  }
  var center='';
  if((mode==='soc'||mode==='charging'||mode==='complete')&&pct!=null){
    center='<span class="gnum"><b>'+Math.round(pct)+'<small>%</small></b>'+(o.limit!=null?'<i>Limit '+o.limit+' %</i>':'')+'</span>';
  } else if(o.glyph){
    center='<span class="gglyph'+(mode==='waking'?' shake':'')+'">'+iconSVG(o.glyph)+'</span>';
  }
  return s+'<span class="gcenter">'+center+'</span>';
}

// hero detail chips; tone: ok | warn | info | ''
function stat(k,val,unit,tone){
  return '<div class="stat'+(tone?' '+tone:'')+'"><div class="k">'+k+'</div><div class="v">'+val+
         (unit?'<small>'+unit+'</small>':'')+'</div></div>';
}
function chargeStats(v,charging){
  if(!v||!charging) return '';
  var c=[];
  if(v.power>0) c.push(stat('Power',   Math.round(v.power), 'kW', 'ok'));
  if(v.amps>0)  c.push(stat('Current', Math.round(v.amps),        'A',  'ok'));
  return c.join('');
}
/* Live AC wall draw (charger actual_current × voltage × phases) in kW — but ONLY when the
   awake car reports a POSITIVE draw. Returns null when the car is asleep (no live charge
   object), didn't report the fields, or the draw is 0/negative. charger_power (battery DC, 0 at
   "Complete") is deliberately not used. The car reports current and voltage PER PHASE, so the
   total wall draw is current × voltage × phases — dropping the phase count undercounts a
   multi-phase charge (an EU 3-phase car reports phases=2, so omitting it halves the kW: a real
   228 V × 3 A × 2 = 1.4 kW reads as 0.7). phases falls back to 1 when the car didn't report it
   (single-phase / unknown). One decimal, trailing ".0" dropped → "1 kW". A null result means
   "nothing is actually being pulled right now", so callers show no active chip. */
function liveKw(s){
  var v=(s&&s.vehicle)||{};
  if(v.actual_amps==null||v.volts==null) return null;
  var w=v.actual_amps*v.volts*(v.phases||1);
  if(w<=0) return null;
  return (w/1000).toFixed(1).replace(/\.0$/,'')+' kW';
}
/* Is the climate telemetry fresh enough to trust a FAST-CHANGING state (defrosting)? It is
   refreshed only while the active poll window is open (a recent evcc/manual command, or
   charging); the moment the car parks and the window closes the climate cache FREEZES, so past
   this age we must not assert a live "defrosting now". last_seen_s is the age of the most recent
   telemetry of any kind (the 10 s charge poll keeps it near 0 while the window is open); the
   per-domain climate lag is bounded by the ~120 s rotation, so 180 s clears one full rotation
   with margin. */
function climateFresh(s){ return !!s && (s.last_seen_s==null || s.last_seen_s<=180); }
/* Overheat (COP) chip — shows the live AC wall draw the car pulls FOR Cabin Overheat
   Protection, in kW (the #99 requirement), and ONLY while that is actually happening:
     • COP is armed (reliable `cop` setting = "On"/"FanOnly"; Off/absent → no chip),
     • the car is awake AND pulling a POSITIVE draw (liveKw — a real measurement), and
     • it is NOT charging (a charging car's wall draw is the charger, shown by the charge chips).
   An armed-but-idle COP draws nothing, so we show NO chip rather than a meaningless
   "OVERHEAT 0 kW" / "On" — being armed (a persistent setting) is not the same as running.
   cop_cooling is intentionally not used: it reads true even below the activation threshold;
   the real wall draw is the honest "COP is running now" signal. */
function copChip(s){
  var c=s&&s.tele&&s.tele.climate, v=s&&s.vehicle;
  if(!c||!c.cop||c.cop==='Off'||c.cop==='Unknown') return '';   // COP must be armed
  if(v&&/charg/i.test(v.status||'')) return '';                 // charging → wall draw is the charger
  var kw=liveKw(s);
  if(!kw) return '';                                            // no positive draw → COP idle → no chip
  var tip='Cabin overheat protection'+(c.cop_temp?' · '+c.cop_temp:'')+' · '+kw+' AC draw';
  return '<div class="stat warn" title="'+esc(tip)+'"><div class="k">Overheat</div>'+
         '<div class="v">'+kw.replace(/ kW$/,'')+'<small>kW</small></div></div>';
}
/* Defrost chip — shown when the car is defrosting (front or rear defroster on, or Max-defrost
   mode engaged) AND a live AC draw is available, since the only thing we surface is the power
   the car pulls for it. No live draw — car asleep or the figure not reported — means no chip;
   the car is never woken to obtain one. Separate from is_climate_on. */
function defrostChip(s){
  var c=s&&s.tele&&s.tele.climate;
  if(!c||!climateFresh(s)) return '';
  if(!(c.front_defrost===true||c.rear_defrost===true||(c.defrost_mode&&c.defrost_mode!=='Off'))) return '';
  var kw=liveKw(s);
  if(!kw) return '';                                           // no live draw → nothing to show
  var p=[]; if(c.front_defrost===true)p.push('front'); if(c.rear_defrost===true)p.push('rear');
  if(c.defrost_mode&&c.defrost_mode!=='Off')p.push(c.defrost_mode.toLowerCase());
  var tip='Defrost'+(p.length?' · '+p.join(', '):'');
  return '<div class="stat info" title="'+esc(tip)+'"><div class="k">Defrost</div>'+
         '<div class="v">'+kw.replace(/ kW$/,'')+'<small>kW</small></div></div>';
}
// battery at/above its target and not charging → starting a charge is a no-op the car only
// rejects as "complete". charge_limit is emitted by /status only when the car reported it, so
// an unknown limit (field absent) leaves the start button live rather than blocking on a guess.
function chargeComplete(v,charging){
  var cur = (v && v.usable_soc!=null) ? v.usable_soc : (v ? v.soc : null);
  return !charging && !!v && v.charge_limit!=null && (cur!=null?cur:0)>=v.charge_limit;
}
// Compact "time ago" for the asleep card (seconds → "<1 min" / "5 min" / "2 h" / "3 d").
function fmtAgo(sec){
  if(sec==null||sec<0) return '';
  sec=Math.floor(sec);
  if(sec<60) return '<1 min';
  var m=Math.round(sec/60); if(m<60) return m+' min';
  var h=Math.floor(m/60);   if(h<48) return h+' h';
  return Math.floor(h/24)+' d';
}

/* ---------- setup shortcuts ---------- */
function scrollToKey(){
  var el = $("rowKey") || $("paneSettings");
  if(el && typeof el.scrollIntoView === 'function'){
    try { el.scrollIntoView({ behavior: 'smooth' }); } catch(e){ el.scrollIntoView(); }
  }
  var btn = $("keyBtn");
  if(btn && typeof btn.focus === 'function') btn.focus();
}

// The hero's one primary action, picked by render(); the gauge calls heroTap().
var heroActFn=null;
function heroTap(){ if(heroActFn) return heroActFn(); }
function setHeroAct(label,icon,fn,busyText){
  heroActFn=busyText?null:fn;
}
var bannerFn=null;
function bannerAction(){ if(isOtaRunning()) return; if(bannerFn) return bannerFn(); }

/* ---------- render ---------- */
// Connection row: tone ('ok' | 'warn' | '') colours the icon dot, status line and signal glyph.
function setRow(key,tone,valueHTML,statusHTML,barsMarkup){
  var d=$(key+'Dot'); if(d) d.className='dot'+(tone?' '+tone:'');
  setHTML($(key+'Val'),valueHTML);
  var st=$(key+'St'); if(st){ st.className='rs'+(tone&&tone!=='err'?' '+tone:''); setHTML(st,statusHTML); }
  var b=$(key+'Bars'); if(b){ b.className='rbar'+(tone?' '+tone:''); setHTML(b,barsMarkup||''); }
}
function lastChips(s){
  var ls=s.last||{}, lsoc=(ls.usable_soc!=null)?Math.round(ls.usable_soc):((ls.soc!=null)?Math.round(ls.soc):null), ago=fmtAgo(s.last_seen_s), chips=[];
  if(lsoc!=null) chips.push(stat('Battery','<span style="color:'+socColor(lsoc)+'">'+lsoc+'</span>','%'));
  if(ago)        chips.push(stat('Idle', ago, ''));
  return {html:chips.join(''), soc:lsoc};
}
function render(s){
  state=s;
  var linked=!!(s.ble&&s.ble.connected), paired=!!s.paired;
  var hasVin=s.vin&&s.vin!=='UNKNOWN', configured=hasVin&&s.key_present;
  var v=s.vehicle, charging=v&&/charg/i.test(v.status||'');
  var safe=!!(s.sys&&s.sys.safe_mode);

  // network row. One row, two possible transports: a wired device reports an `eth` object
  // (present ONLY when the wire carries the lease, see logic/status_model.hpp) and no SSID/RSSI,
  // so bars and a dBm reading would be fabricated there — it gets the negotiated link speed
  // instead, which is the equivalent "how good is this link" fact.
  var w=s.wifi||{}, e=s.eth;
  var wi=$("wifiIcon"), ei=$("ethIcon");
  if(e && e.link){
    $("wifiLbl").textContent='Ethernet';
    if(wi&&wi.classList) wi.classList.add('hide'); if(ei&&ei.classList) ei.classList.remove('hide');
    setRow('wifi','ok', e.speed?esc(e.speed+' Mbit'+(e.full_duplex?' full duplex':' half duplex')):'Link up', 'Connected', '');
  } else {
    $("wifiLbl").textContent = w.std || 'Wi-Fi';   // standard sits in the label, e.g. "Wi-Fi 4"
    if(wi&&wi.classList) wi.classList.remove('hide'); if(ei&&ei.classList) ei.classList.add('hide');
    if(w.ssid||s.ip){
      var wr=num(w.rssi);
      setRow('wifi','ok', w.ssid?esc(w.ssid):'Connected', 'Connected'+(wr!=null?' · '+wr+' dBm':''), (wr!=null)?barsHTML(wr):'');
    } else {
      // not connected — the bars fill up one after another while the device keeps reconnecting.
      // setHTML keeps the same bar nodes between polls so the fill animation doesn't restart.
      setRow('wifi','warn','Searching…','Reconnecting',searchBarsHTML());
    }
  }

  // re-pair / Safe Mode notice
  var rb=$("reauthBanner"), ba=$("bannerAct");
  if(s.reauth && !paired){
    rb.classList.add('show');
    setHTML(rb.querySelector('.bt'),'<b>Key was reset</b> — The vehicle removed this device’s key, so a fresh one was generated automatically. Approve the new pairing on your Tesla’s touchscreen.');
    if(ba) ba.textContent='Open setup'; bannerFn=function(){ scrollToKey(); };
  } else if(safe){
    rb.classList.add('show');
    setHTML(rb.querySelector('.bt'),'<b>Safe Mode active</b> — Vehicle Bluetooth, commands, and telemetry are stopped. Use this recovery dashboard to inspect diagnostics or update firmware.');
    if(ba) ba.textContent='Check for update'; bannerFn=otaCheck;
  } else { rb.classList.remove('show'); bannerFn=null; }

  // hero — single source of overall status. The gauge markup is written only when it actually
  // changes (setHTML), so a running pulse/flow/spinner keeps its phase instead of restarting
  // every poll.
  var hic=$("hicon"), hl=$("hlabel"), hs=$("hsub"), hst=$("hstats");
  var g={mode:'neutral', pct:null, glyph:null, busy:false, limit:null}, head='', sub='', chips='';
  var act=null, actLabel='', actIcon='bolt', busyText=null;
  $("hero").classList.remove('hide');
  if(paired&&v){
    if(waking){ waking=false; clearTimeout(wakeTimeout); }   // car is awake & reporting — stop the spinner
    var soc=(v.usable_soc!=null?num(v.usable_soc):num(v.soc))||0;
    // "Charge complete" = battery reached its target and not charging; the start tap is gated
    // (the car would only reject a charge_start here as "complete"). See chargeComplete().
    var complete=chargeComplete(v,charging);
    g={mode:charging?'charging':(complete?'complete':'soc'), pct:soc, busy:chgBusy, limit:num(v.charge_limit)};
    head=complete?'Charge complete':(v.status||'Idle');
    chips=chargeStats(v,charging)+copChip(s)+defrostChip(s);
    if(chgBusy) busyText='Sending command…';
    else if(!complete){ act=toggleCharge; actLabel=charging?'Stop charging':'Start charging'; }
  } else if(paired){
    var lk=lastChips(s);
    if(waking){
      g={mode:'waking', pct:lk.soc, glyph:'alarm'};
      head='Waking up…'; sub='Reaching your Tesla over Bluetooth…'; busyText='Waking up…';
    } else if(s.link==='asleep'){
      // Proven reachable-but-sleeping: the always-on VCSEC health poll answered recently, so
      // "asleep" is a fact and the wake button is meaningful. Last-known battery + how long the
      // car has been asleep come from the retained cache — shown without waking the car.
      // No Overheat/Defrost chips: their value is the live AC draw, which only exists while the
      // car is awake and reporting, and we never wake the car to obtain it.
      g={mode:'sleep', pct:lk.soc, glyph:'moon'};
      head='Vehicle asleep'; sub='Tap the icon to wake the car.'; chips=lk.html;
      act=wakeCar; actLabel='Wake the car'; actIcon='alarm';
    } else if(s.link==='idle'){
      // Reachable over BLE but NOT provably asleep: polling stopped to let the car sleep and the
      // VCSEC sleep flag hasn't held ASLEEP long enough to confirm. We honestly don't know, so
      // we must NOT claim "asleep": a neutral "Parked" card that still offers the wake button.
      g={mode:'sleep', pct:lk.soc, glyph:'car'};
      head='Parked'; sub='No live reading — tap the icon to wake the car.'; chips=lk.html;
      act=wakeCar; actLabel='Wake the car'; actIcon='alarm';
    } else if((s.ble&&s.ble.connect_fail)>=2){
      // Paired, but the BLE link won't come up despite repeated attempts. Surface WHY.
      // car_connectable===false ⇒ the car is at its ~3-device BLE limit (no slot for us).
      g={mode:'empty', glyph:'bluetooth'};
      head='Connection failed';
      sub=(s.ble.car_connectable===false)
        ? 'The car has too many Bluetooth devices connected.'
        : 'Move the device closer, or disconnect other devices using the car.';
    } else if(s.link==='unreachable'){
      // heard before, now stale: drove off / out of range / deep sleep
      g={mode:'empty', glyph:'bolt'};
      head='Vehicle unreachable'; sub='Bring the device within Bluetooth range.'; chips=lk.html;
    } else {
      // link==='unknown' (nothing heard since boot — the on-demand link hasn't completed a signed
      // round-trip yet). Only claim "Bluetooth connected" when the momentary GATT link is actually
      // up; otherwise the BLE row reads "Disconnected" and the subtitle would contradict it.
      g={mode:'checking'};
      head='Checking status…';
      sub=linked ? 'Bluetooth connected — checking status…' : 'Reaching your Tesla over Bluetooth…';
      chips=lk.html;
    }
  } else if(!configured){
    g={mode:'neutral', glyph:hasVin?'key':'pencil'};
    head='Set up needed'; sub=hasVin?'Generate a security key in Setup.':'Add the vehicle VIN in Setup to begin.';
    act=hasVin?genKey:editVin; actLabel=hasVin?'Generate key':'Add VIN'; actIcon=hasVin?'key':'pencil';
  } else if(linked){
    g={mode:'neutral', glyph:'link'};
    head='Pairing'; sub='Approve the request on your Tesla’s touchscreen.';
  } else if((s.ble&&s.ble.connect_fail)>=2){
    // The car WAS found (advert heard, name matches the VIN) but every connection attempt times
    // out. Don't blame range. car_connectable===false → the car is at its BLE connection limit;
    // otherwise → weak signal or another proxy contending for the link.
    g={mode:'empty', glyph:'bluetooth'};
    head='Connection failed';
    sub=(s.ble.car_connectable===false)
      ? 'The car has too many Bluetooth devices connected.'
      : 'Move the device closer, or disconnect other devices using the car.';
  } else {
    g={mode:'neutral', glyph:'search'};
    head='Looking for your car'; sub='Bring the device within Bluetooth range.';
  }
  // Safe Mode stops Bluetooth and commands, so it never offers a vehicle action.
  if(safe){ act=null; busyText=null; }
  // The gauge button's label replaces its content for screen readers, so it carries the level.
  var gm=gaugeHTML(g), lvl=(g.pct!=null)?' · battery '+Math.round(g.pct)+' %':'';
  setHTML(hic, act ? '<button type="button" class="gbtn" onclick="heroTap()" aria-label="'+esc(actLabel+lvl)+'" title="'+esc(actLabel)+'">'+gm+'</button>'
                   : (busyText ? '<button type="button" class="gbtn" disabled aria-label="'+esc(busyText+lvl)+'">'+gm+'</button>' : gm));
  setHTML(hl,'<span>'+esc(head)+'</span>');
  hs.textContent=sub;
  hs.style.display=sub?'':'none';
  setHTML(hst,chips);
  setHeroAct(actLabel,actIcon,act,busyText);

  // setup tiles ----------------------------------------------------
  $("vehVal").innerHTML=hasVin?esc(s.vin):'<span class="ph">Not set</span>';
  var pairedTxt=s.paired_at?('Paired '+new Date(s.paired_at*1000).toLocaleDateString(undefined,{year:'numeric',month:'short',day:'numeric'})):'Paired with the vehicle';
  setSub("vehSub", paired?pairedTxt:(hasVin?'Not paired yet':'Add the VIN to begin'), '');
  var vinTx=hasVin?'Change VIN':'Add VIN';
  var vb=$("vinBtn"); if(vb){ vb.setAttribute('aria-label',vinTx); vb.setAttribute('title',vinTx); }

  // bluetooth — link to the car. WHICH state to show is decided by bleRowFromStatus() above (the
  // JS half of main/logic/ble_row.hpp, kept honest by scripts/check-ble-row-parity.sh); this
  // block only renders the decision. Note what is NOT consulted here: ble.scanning. The radio
  // scans for reasons with no deadline, and keying the label off that flag while the countdown
  // came from the phase is what used to make the two disagree.
  var ble=s.ble||{};
  var d=bleRowFromStatus(s);
  if(d.row==='listing'){
    // No VIN: the device never connects/enrols (pairing is gated off), it only discovers.
    // List every nearby Tesla with its signal.
    var devs=ble.devices||[];
    setRow('ble','', devs.length+' Tesla'+(devs.length===1?'':'s')+' nearby'+
      '<span class="btlist">'+devs.map(function(x){ var r=num(x.rssi); return '<span>'+esc(x.addr)+(r!=null?' · '+r+' dBm':'')+'</span>'; }).join('')+'</span>',
      'Add a VIN to pair', '');
  } else if(d.row==='discovering'){
    // No VIN and nothing seen yet — the one "Searching…" with nothing to count down.
    setRow('ble','warn','Searching…','Scanning for Teslas',searchBarsHTML());
  } else if(d.row==='linked'){
    // The GATT link is up. Stateless (paired but no signed round-trip yet) shows the amber wave
    // instead of healthy green bars — the link is connected yet knows nothing.
    var br=num(ble.rssi);
    setRow('ble', d.stateless?'warn':'ok', ble.addr?'<span class="mono">'+esc(ble.addr)+'</span>':'Connected',
      (d.stateless?'Connected · status unknown':'Connected')+(br!=null?' · '+br+' dBm':''),
      d.stateless?waveBarsHTML():(br!=null?barsHTML(br):''));
  } else if(d.row==='failed'){
    // Car found but the link keeps timing out — say so rather than "Searching…". Real bars + dBm
    // (the signal is fine, the link slot is not). Null-safe "strongest nearby advert" fallback:
    // take the first, then the least-negative RSSI; empty devices[] → null.
    var sr=num((ble.rssi!=null)?ble.rssi:(ble.devices||[]).reduce(function(a,x){return (a==null||x.rssi>a)?x.rssi:a;},null));
    setRow('ble','warn', (ble.car_connectable===false)?'At connection limit':'Connection failed',
      'Failed'+(sr!=null?' · '+sr+' dBm':''), barsHTML(sr));
  } else if(d.row==='scanning'){
    // A bounded connect attempt is running. The countdown lives in its own node (painted by
    // paintCd) so the bar fill animation is never restarted by the 1 s tick.
    setRow('ble','warn','Searching…','Connecting · '+cdHTML(d.cd),searchBarsHTML(true));
  } else {
    // Link down, next attempt scheduled (or none). Outlined empty bars hold the row's shape.
    setRow('ble','','Disconnected', d.cd==='retries'?'Waiting · '+cdHTML(d.cd):'Idle', emptyBarsHTML());
  }
  // Countdown clock from this push, then paint immediately: setHTML may have just replaced the
  // row, leaving a fresh, empty .cd the 1 s ticker wouldn't fill until its next tick.
  cdSync(d.cd,num(ble.phase_s));
  paintCd();

  // MQTT — Home Assistant bridge
  var mq=s.mqtt||{};
  if(!mq.configured) setRow('mqtt','','<span class="ph">Not configured</span>','Disabled');
  else if(mq.connected) setRow('mqtt','ok',esc(mq.broker||''),'Connected'+(mq.tls?' · secured':''));
  else { setRow('mqtt','warn',esc(mq.broker||''),'Disconnected · '+esc(mq.error||'not connected')); }

  // Syslog — UDP diag-log forwarder. Delivery is gated on DNS only (resolved); reachability is an
  // advisory ping hint, so a resolved-but-not-answering host still shows the destination, flagged.
  var sy=s.syslog||{};
  if(!sy.configured) setRow('syslog','','<span class="ph">Not configured</span>','Disabled');
  else if(sy.error){ setRow('syslog','warn',esc(sy.host?(sy.host+':'+(sy.port||514)):''),'Error · '+esc(sy.error)); }
  else if(sy.resolved){
    setRow('syslog',sy.reachable?'ok':'warn',esc(sy.host+':'+(sy.port||514)),sy.reachable?'Sending · UDP':'Not answering ping');
  }
  else { setRow('syslog','warn','Resolving…','Resolving'); }

  // header + firmware pane
  renderIpLine();
  renderFwVer();

  if(s.ota && typeof s.ota.channel === 'string'){
    var sChan = s.ota.channel === 'dev' ? 'dev' : 'release';
    if(otaChannelInFlight || otaChannelTarget){
      if(sChan === otaChannelTarget){
        otaChannelTarget = null;
      }
    } else {
      otaChannel = sChan;
      otaChannelInitialSet = true;
    }
    renderChanMenu();
  } else if(!otaChannelInitialSet && s.version){
    otaChannel = /-dev/i.test(s.version) ? 'dev' : 'release';
    otaChannelInitialSet = true;
    renderChanMenu();
  } else {
    renderChanMenu();
  }
  renderVerLink();

  // key
  var keyTx=s.key_present?'Regenerate key':'Generate key';
  var kbBtn=$("keyBtn");
  if(kbBtn){ kbBtn.setAttribute('aria-label',keyTx); kbBtn.setAttribute('title',keyTx); }
  if(s.key_present){
    $("keyVal").innerHTML=esc(s.key_fingerprint||'');
    var created=s.key_created?('Created '+new Date(s.key_created*1000).toLocaleDateString(undefined,{year:'numeric',month:'short',day:'numeric'})):'';
    if(s.reauth&&!paired) setSub("keySub",'New key — approve it on the touchscreen','warn');
    else if(paired) setSub("keySub",created?created+' · paired':'Paired with the vehicle','ok');
    else setSub("keySub",(created?created+' · ':'')+'not paired yet','');
  } else {
    $("keyVal").innerHTML='<span class="ph">Not generated</span>';
    setSub("keySub",'No key yet','');
  }

  if(typeof syncOtaUi === 'function') syncOtaUi();
}
function setSub(id,txt,cls){ var e=$(id); if(e){ e.className='rs'+(cls?' '+cls:''); e.textContent=txt; } }

function formatVerVerbatim(v){
  var raw = String(v || '').trim();
  if(!raw) return '';
  return (raw.charAt(0) === 'v' || raw.charAt(0) === 'V') ? raw : ('v' + raw);
}

function renderIpLine(){
  var el = $("ipline");
  if(!el) return;
  var host = (typeof location !== 'undefined' && location.host) ? location.host : '';
  var ip = (state && state.ip) ? state.ip : host;
  var ver = formatVerVerbatim(state && state.version);
  el.textContent = ip + (ver ? (' · ' + ver) : '');
}

function renderFwVer(){
  var fv = $("fwVer");
  if(!fv) return;
  var ver = formatVerVerbatim(state && state.version);
  fv.textContent = ver || '—';
}

function renderVerLink(){
  var vl = $("verLink");
  if(vl){
    var chan = otaChannel || 'release';
    var isDev = (chan === 'dev');
    var chanLabel = isDev ? 'Development' : 'Release';
    vl.title = 'Change update channel (current: ' + chanLabel + ')';
  }
  var cb = $("fwCheckBtn");
  if(cb){
    var isAvail = !!otaAvail;
    cb.className = 'ib' + (isAvail ? ' avail' : '');
    if(isAvail){
      cb.title = 'Update ' + otaAvail + ' available — tap to install';
      cb.setAttribute('aria-label', 'Install firmware update ' + otaAvail);
    } else {
      cb.title = 'Check for firmware updates';
      cb.setAttribute('aria-label', 'Check for firmware updates');
    }
  }
}

// tap the SOC ring to start/stop charging
function toggleCharge(){
  if(chgBusy) return;
  var vin=state&&state.vin; if(!vin||vin==='UNKNOWN'){ toast('No VIN configured','err'); return; }
  var v=state&&state.vehicle, isCharging=!!(v&&/charg/i.test(v.status||''));
  // belt-and-suspenders: the start button is already disabled when complete, but never fire a
  // pointless charge_start at/above the charge limit even if this is reached another way.
  if(chargeComplete(v,isCharging)){ toast('Battery is already fully charged','info'); return; }
  var cmd=isCharging?'charge_stop':'charge_start';
  chgBusy=true; if(state)render(state);
  toast(isCharging?'Stopping charge…':'Starting charge…', 'load', 'charge');
  return requestJsonResult('/api/1/vehicles/'+encodeURIComponent(vin)+'/command/'+cmd,{method:'POST'})
    .then(function(res){
      var j=res.json;
      if(j && j.response && typeof j.response.result === 'boolean'){
        if(j.response.result){ toast(isCharging?'Charging stopped':'Charging started', 'ok', 'charge'); return; }
        var f=chargeFailMsg((j.response.reason)||'', isCharging);
        toast(f.msg, f.type, 'charge');
        return;
      }
      if(!res.ok && res.status){
        toast('Command failed (HTTP '+res.status+')', 'err', 'charge');
        return;
      }
      toast('Command failed — is the car in range?', 'err', 'charge');
    })
    .catch(function(){ toast('Command failed — is the car in range?', 'err', 'charge'); })
    .then(function(){ chgBusy=false; poll(); });
}
// turn the device's failure reason into a clear message; the car rejecting a command
// (e.g. "complete") is not a connectivity problem, so don't blame the BLE range.
function chargeFailMsg(reason,isCharging){
  var r=String(reason).toLowerCase();
  if(r.indexOf('not reachable')>=0||r.indexOf('timed out')>=0||r.indexOf('unreachable')>=0)
    return {msg:'Car not reachable — is it in range?', type:'err'};
  if(r.indexOf('complete')>=0)        return {msg:'Charging is already complete', type:'info'};
  if(r.indexOf('already_set')>=0||r.indexOf('already set')>=0)
                                      return {msg:'Setting already active on vehicle', type:'info'};
  if(r.indexOf('not_charging')>=0||r.indexOf('not charging')>=0)
                                      return {msg:'The car isn’t charging', type:'info'};
  if(r.indexOf('is_charging')>=0)     return {msg:'The car is already charging', type:'info'};
  if(r.indexOf('disconnected')>=0||r.indexOf('cable')>=0||r.indexOf('unplug')>=0)
                                      return {msg:'No charge cable connected', type:'info'};
  var bare=String(reason).replace(/^.*action failed:\s*/i,'').trim();
  return {msg:(isCharging?'Couldn’t stop charging':'Couldn’t start charging')+(bare?' — '+bare:''), type:'err'};
}

/* ---------- sheets (replace prompt()/confirm()) ----------
   askText → Promise<string|null>, askConfirm → Promise<boolean>. One sheet at a time; opening
   another settles the first as cancelled. */
var askResolve=null, askCancelValue=null, askReturnFocus=null, askHintFn=null, askValidateFn=null;
// While a sheet or the OTA dialog is open the page behind it is inert: aria-modal alone neither
// stops Tab from walking into the rest of the page nor keeps screen readers inside the dialog.
// Derived from all three dialogs, so closing one while another is still up keeps the page locked.
// Call it after showing/hiding a dialog and before moving focus back: an inert node can't take it.
// Both can be open at once (an OTA check finishing while a sheet is up). The sheet is always the
// top layer (CSS z-index), so Escape closes it first, and the OTA dialog never takes its focus.
function isOpen(id){
  var m=$(id);
  return !!(m && m.classList && !m.classList.contains('hide'));
}
function syncModal(){
  var chanOpen = isOpen('chanModal');
  var open = isOpen('askModal') || isOpen('otaModal') || chanOpen;
  if(typeof document!=='undefined'){
    if(document.documentElement&&document.documentElement.classList) document.documentElement.classList.toggle('modal-open',open);
    if(document.body&&document.body.classList) document.body.classList.toggle('modal-open',open);
  }
  var w=$("wrap"); if(w) w.inert=open;
  // The OTA dialog sits under an open sheet: keep Tab from reaching its buttons.
  var om=$("otaModal"); if(om) om.inert=isOpen('askModal') || chanOpen;
  var cm=$("chanModal"); if(cm) cm.inert=isOpen('askModal');
}
function askValidate(){
  var inp=$("askInput"), ok=$("askOk"), err=$("askErr");
  askPaintHint();
  if(!inp) return true;
  var msg=(askValidateFn&&typeof askValidateFn==='function')?askValidateFn(inp.value||''):null;
  var bad=!!msg;
  if(err){
    err.textContent=bad?msg:'';
    err.classList.toggle('hide',!bad);
  }
  inp.classList.toggle('invalid',bad);
  if(bad) inp.setAttribute('aria-invalid','true');
  else if(inp.removeAttribute) inp.removeAttribute('aria-invalid');
  if(ok) ok.disabled=bad;
  return !bad;
}
function askOpen(o,cancelValue){
  if(isOtaBusy()) return Promise.resolve(cancelValue);
  if(askResolve) askClose(askCancelValue);
  askReturnFocus=(typeof document!=='undefined')?document.activeElement:null;
  askCancelValue=cancelValue;
  var text=!!o.input;
  $("askTitle").textContent=o.title||'';
  var ic=$("askIcon"); if(ic&&ic.classList) ic.classList.toggle('hide',!o.destructive||text);
  var p=$("askText"); if(p){ p.textContent=o.text||''; if(p.classList) p.classList.toggle('hide',!o.text); }
  var f=$("askField"); if(f&&f.classList) f.classList.toggle('hide',!text);
  var dt=$("askDetail"); if(dt){ dt.textContent=o.detail||''; if(dt.classList) dt.classList.toggle('hide',!o.detail); }
  var n=$("askNote"); if(n){ n.textContent=o.note||''; n.className='note'+(o.destructive?' danger':''); n.hidden=!o.note; }
  var ok=$("askOk"); if(ok) ok.textContent=o.okLabel||'Save';
  var cc=$("askCancel"); if(cc) cc.textContent=o.cancelLabel||'Cancel';
  var inp=$("askInput");
  askHintFn=null;
  askValidateFn=null;
  if(text&&inp){
    $("askLabel").textContent=o.label||'';
    inp.value=o.value||'';
    inp.placeholder=o.placeholder||'';
    inp.className=o.mono?'mono':'';
    if(o.maxLength) inp.setAttribute('maxlength',String(o.maxLength)); else if(inp.removeAttribute) inp.removeAttribute('maxlength');
    askHintFn=o.hint||null;
    askValidateFn=o.validate||null;
    askValidate();
  } else if(ok){
    ok.disabled=false;
  }
  var m=$("askModal"); if(m&&m.classList) m.classList.remove('hide');
  syncModal();
  // Adaptive focus: on desktop (pointer: fine), focus the input immediately and select text
  // so typing and Enter work right away. On mobile/touch, focus the card container so
  // the virtual keyboard does not jump.
  if(text){
    var isDesktop = typeof window !== 'undefined' && typeof window.matchMedia === 'function' && window.matchMedia('(pointer: fine)').matches;
    if(isDesktop && inp && typeof inp.focus === 'function'){
      inp.focus();
      if(typeof inp.select === 'function') inp.select();
    } else {
      var card=m?(m.querySelector?m.querySelector('.modal-card'):null):null;
      if(!card) card=m;
      if(card&&typeof card.focus==='function') card.focus({preventScroll:true});
    }
  } else {
    var target=o.destructive?cc:ok;
    if(target&&typeof target.focus==='function') target.focus();
  }
  return new Promise(function(resolve){ askResolve=resolve; });
}
function askPaintHint(){
  var h=$("askHint"), inp=$("askInput"); if(!h) return;
  h.textContent=(askHintFn&&inp)?askHintFn(inp.value||''):'';
}
function askClose(value){
  var m=$("askModal"); if(m&&m.classList) m.classList.add('hide');
  syncModal();
  askValidateFn=null;
  var err=$("askErr"), inp=$("askInput"), ok=$("askOk");
  if(err){ err.textContent=''; err.classList.add('hide'); }
  if(inp){ inp.classList.remove('invalid'); if(inp.removeAttribute) inp.removeAttribute('aria-invalid'); }
  if(ok) ok.disabled=false;
  var r=askResolve; askResolve=null;
  var back=askReturnFocus; askReturnFocus=null;
  // Back to where the sheet was opened from — unless that is on the page and the OTA dialog is
  // still up (the page is inert then): focus that dialog instead of losing focus to <body>.
  var w=$("wrap"), onPage=!!(back&&w&&typeof w.contains==='function'&&w.contains(back));
  if(isOpen('otaModal')&&(!back||onPage)) back=$("otaInstall");
  if(back&&typeof back.focus==='function') back.focus();
  if(r) r(value);
}
function askSubmit(){
  if(!askResolve) return;
  if(askCancelValue===false){ askClose(true); return; }
  if(!askValidate()){
    var inp=$("askInput");
    if(inp && typeof inp.focus==='function') inp.focus();
    return;
  }
  var inp=$("askInput"); askClose(inp?String(inp.value):'');
}
function askText(o){ o.input=true; return askOpen(o,null); }
function askConfirm(o){ return askOpen(o,false); }

// Popup editing contract: activating an INACTIVE text field selects its complete
// value, so replacing a VIN, broker or server is one paste. A tap/click in the ALREADY ACTIVE field
// keeps native caret placement.
function selectModalFieldContents(target){
  if(!target || typeof target.matches !== 'function' || typeof target.select !== 'function') return false;
  if(!target.matches('.modal-card textarea, .modal-card input:not([type="checkbox"]):not([type="radio"]):not([type="file"])'))
    return false;
  try { target.select(); return true; } catch(e){ return false; }
}

function selectModalFieldOnActivation(target, wasActive){
  return !wasActive && selectModalFieldContents(target);
}

function wireModalFieldSelection(doc){
  if(!doc || typeof doc.addEventListener !== 'function') return;
  var pointerTarget = null;
  var pointerWasActive = false;
  doc.addEventListener('pointerdown', function(event){
    if(!event.target || typeof event.target.matches !== 'function' ||
       !event.target.matches('.modal-card textarea, .modal-card input:not([type="checkbox"]):not([type="radio"]):not([type="file"])')) return;
    pointerTarget = event.target;
    pointerWasActive = (doc.activeElement === event.target);
    if(pointerWasActive && typeof event.target.setSelectionRange === 'function'
       && event.target.selectionStart !== event.target.selectionEnd){
      var caret = (event.target.selectionEnd != null) ? event.target.selectionEnd : (event.target.value ? event.target.value.length : 0);
      try { event.target.setSelectionRange(caret, caret); } catch(e){}
    }
  });
  doc.addEventListener('focusin', function(event){
    selectModalFieldOnActivation(event.target, false);
  });
  doc.addEventListener('click', function(event){
    if(event.target === pointerTarget)
      selectModalFieldOnActivation(event.target, pointerWasActive);
    pointerTarget = null;
  });
}

/* ---------- config actions ---------- */
// Byte length as the firmware counts it (UTF-8), not UTF-16 code units.
function utf8Len(v){
  if(typeof TextEncoder!=='undefined') return new TextEncoder().encode(v).length;
  return unescape(encodeURIComponent(v)).length;
}
function vinValid(v){return /^[A-HJ-NPR-Z0-9]{17}$/i.test(v)}
function editVin(){
  if(isOtaBusy()) return Promise.resolve();
  var cur=(state&&state.vin&&state.vin!=='UNKNOWN')?state.vin:'';
  var hasKey=!(state&&state.key_present===false);
  var v;
  // No maxlength: a pasted VIN with stray spaces is trimmed below instead of silently cut short.
  return askText({
    title:'Vehicle VIN', label:'VIN', value:cur, placeholder:'17 characters', mono:true,
    hint:function(x){ return x.trim().length+' / 17'; },
    validate:function(x){
      var s=(x||'').trim();
      if(!s) return 'VIN is required (17 characters)';
      if(s.length!==17) return 'Must be exactly 17 characters ('+s.length+' / 17)';
      if(/[IOQ]/i.test(s)) return 'Letters I, O, Q are not allowed in VINs';
      if(!/^[A-Za-z0-9]+$/.test(s)) return 'Only letters and digits are allowed';
      if(!vinValid(s)) return 'Invalid VIN format';
      return null;
    },
    note:hasKey?'Changing the VIN generates a new security key and clears the stored pairing.'
               :'The 17-character VIN is shown on the Tesla touchscreen under Controls → Software.',
    destructive:hasKey&&!!cur, okLabel:'Save VIN'
  }).then(function(input){
    if(input==null) return false;
    v=input.trim().toUpperCase();
    if(!vinValid(v)){ toast('Invalid VIN — must be 17 characters','err'); return false; }
    if(v===(state&&state.vin)){ toast('VIN unchanged','info'); return false; }
    // The page kept polling while the sheet was open: decide on the key from the current state.
    if(state&&state.key_present===false) return true;
    return askConfirm({
      title:'Change the VIN?', destructive:true,
      text:'This generates a new security key and clears the stored pairing. You must re-pair with the vehicle.',
      detail:(cur||'Not set')+' → '+v, okLabel:'Change VIN', cancelLabel:'Keep current'
    });
  }).then(function(go){
    if(!go) return;
    toast('Saving VIN…', 'load', 'vin');
    return requestJsonResult('/set_vin',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({vin:v})})
      .then(function(res){
        var o=res.json&&res.json.response;
        if(o && typeof o.result === 'boolean' && typeof o.reason === 'string'){
          if(!o.result){ toast(o.reason||'Failed to save VIN', 'err', 'vin'); return; }
          if(/no reboot|unchanged/i.test(o.reason||'')){ toast('VIN unchanged', 'info', 'vin'); return; }
          toast('VIN saved · rebooting', 'ok', 'vin');
          return;
        }
        var msg=(!res.ok && res.status)?('Failed to save VIN (HTTP '+res.status+')'):'Failed to save VIN — no change was confirmed';
        toast(msg, 'err', 'vin');
      })
      .catch(function(){toast('Failed to save VIN — no change was confirmed', 'err', 'vin')});
  });
}
function editMqtt(){
  if(isOtaBusy()) return Promise.resolve();
  var cur=(state&&state.mqtt&&state.mqtt.broker)?state.mqtt.broker:'';
  return askText({
    title:'MQTT broker', label:'Broker (IP:PORT or URI)', value:cur, placeholder:'192.0.2.20:1883', mono:true,
    hint:function(){ return 'Empty disables MQTT'; },
    validate:function(x){
      var v=(x||'').trim();
      if(!v) return null;
      if(/[ \t\r\n]/.test(v)) return 'Invalid broker — spaces not allowed';
      if(utf8Len(v)>120) return 'Broker too long (max 120 bytes)';
      var auth=v, scheme=auth.indexOf('://');
      if(scheme>=0) auth=auth.slice(scheme+3);
      var colon=auth.lastIndexOf(':');
      if(colon<=0) return 'Invalid broker — use host:port';
      var host=auth.slice(0,colon), port=auth.slice(colon+1);
      if(!host||!port||!/^\d{1,5}$/.test(port)) return 'Invalid port (1–65535)';
      var p=parseInt(port,10);
      if(p<1||p>65535) return 'Port out of range (1–65535)';
      return null;
    },
    note:'For Home Assistant. Saved credentials stay hidden; leaving the shown host unchanged keeps them.',
    okLabel:'Save & connect'
  }).then(function(v){
    if(v==null) return;
    v=v.trim();
    if(v && v.indexOf(' ')>=0){ toast('Invalid broker — use IP:PORT', 'err', 'mqtt'); return; }
    if(v===cur){ toast(v?'MQTT broker unchanged':'MQTT already disabled', 'info', 'mqtt'); return; }
    toast(v?'Saving MQTT broker…':'Disabling MQTT…', 'load', 'mqtt');
    return requestJsonResult('/set_mqtt',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({broker:v})})
      .then(function(res){
        var o=res.json&&res.json.response;
        if(o && typeof o.result === 'boolean' && typeof o.reason === 'string'){
          if(!o.result){ toast(o.reason||'Failed to save MQTT broker', 'err', 'mqtt'); return; }
          if(/no reboot|unchanged|already/i.test(o.reason||'')){ toast(v?'MQTT broker unchanged':'MQTT already disabled', 'info', 'mqtt'); return; }
          toast('Saved · rebooting', 'ok', 'mqtt');
          return;
        }
        var msg=(!res.ok && res.status)?('Failed to save MQTT broker (HTTP '+res.status+')'):'Failed to save MQTT broker — no change was confirmed';
        toast(msg, 'err', 'mqtt');
      })
      .catch(function(){toast('Failed to save MQTT broker — no change was confirmed', 'err', 'mqtt')});
  });
}
function editSyslog(){
  if(isOtaBusy()) return Promise.resolve();
  var sy=state&&state.syslog, cur=(sy&&sy.host)?(sy.host+':'+(sy.port||514)):'';
  return askText({
    title:'Syslog server', label:'Server (IP:PORT)', value:cur, placeholder:'192.0.2.30:514', mono:true,
    hint:function(){ return 'Empty disables it'; },
    validate:function(x){
      var v=(x||'').trim();
      if(!v) return null;
      if(/[ \t\r\n]/.test(v)) return 'Invalid server — spaces not allowed';
      if(utf8Len(v)>120) return 'Server too long (max 120 bytes)';
      if(/^[a-zA-Z]+:\/\//.test(v)) return 'Invalid server — no scheme allowed';
      var colon=v.lastIndexOf(':');
      if(colon>=0){
        var host=v.slice(0,colon), port=v.slice(colon+1);
        if(!host||!port||!/^\d{1,5}$/.test(port)) return 'Invalid port (1–65535)';
        var p=parseInt(port,10);
        if(p<1||p>65535) return 'Port out of range (1–65535)';
      }
      return null;
    },
    note:'The diagnostic log is sent over UDP to this server.',
    okLabel:'Save'
  }).then(function(v){
    if(v==null) return;
    v=v.trim();
    if(v && v.indexOf(' ')>=0){ toast('Invalid server — use IP:PORT', 'err', 'syslog'); return; }
    if(v===cur){ toast(v?'Syslog server unchanged':'Syslog already disabled', 'info', 'syslog'); return; }
    toast(v?'Saving Syslog server…':'Disabling Syslog…', 'load', 'syslog');
    return requestJsonResult('/set_syslog',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({server:v})})
      .then(function(res){
        var o=res.json&&res.json.response;
        if(o && typeof o.result === 'boolean' && typeof o.reason === 'string'){
          if(!o.result){ toast(o.reason||'Failed to save Syslog server', 'err', 'syslog'); return; }
          if(/no reboot|unchanged|already/i.test(o.reason||'')){ toast(v?'Syslog server unchanged':'Syslog already disabled', 'info', 'syslog'); return; }
          toast('Saved · rebooting', 'ok', 'syslog');
          return;
        }
        var msg=(!res.ok && res.status)?('Failed to save Syslog server (HTTP '+res.status+')'):'Failed to save Syslog server — no change was confirmed';
        toast(msg, 'err', 'syslog');
      })
      .catch(function(){toast('Failed to save Syslog server — no change was confirmed', 'err', 'syslog')});
  });
}
function wakeStop(){ waking=false; clearTimeout(wakeTimeout); }
function wakeCar(){
  if(waking) return;
  var vin=state&&state.vin;
  if(!vin||vin==='UNKNOWN'){ toast('No VIN configured','err'); return; }
  waking=true; if(state)render(state);                       // ring starts spinning immediately
  clearTimeout(wakeTimeout);
  // safety net: stop spinning if no charge data shows up in time
  wakeTimeout=setTimeout(function(){ if(waking){ wakeStop(); toast('Still asleep — try again', 'info', 'wake'); poll(); } }, 90000);
  toast('Waking the car…', 'load', 'wake');
  return requestJsonResult('/api/1/vehicles/'+encodeURIComponent(vin)+'/command/wake_up',{method:'POST'})
    .then(function(res){
      var j=res.json;
      if(j && j.response && typeof j.response.result === 'boolean'){
        if(j.response.result){
          toast('Wake sent · waiting for the car…', 'load', 'wake');
        } else {
          wakeStop();
          var r=(j.response.reason||'').trim();
          toast(r?('Wake failed — '+r):'Wake failed — is the car in range?', 'err', 'wake');
        }
        poll();
        return;
      }
      wakeStop();
      var msg=(!res.ok && res.status)?('Wake failed (HTTP '+res.status+')'):'Wake failed — is the car in range?';
      toast(msg, 'err', 'wake');
      poll();
    })
    .catch(function(){ wakeStop(); toast('Wake failed — is the car in range?', 'err', 'wake'); poll(); });
}
function genKey(){
  if(isOtaBusy()) return Promise.resolve();
  var keyKnown = state && typeof state.key_present === 'boolean';
  var hasKey = keyKnown ? state.key_present : true;
  var ask = hasKey ? askConfirm({
    title:'Regenerate the security key?', destructive:true,
    text:'The current key is invalidated and you must re-pair with the vehicle.',
    detail:(state&&state.key_fingerprint)||'', okLabel:'Regenerate key', cancelLabel:'Keep current'
  }) : Promise.resolve(true);
  return ask.then(function(go){
    if(!go) return;
    toast('Generating new key…', 'load', 'key');
    var query = (keyKnown && state.key_present) ? '?force=1' : '';
    return requestJsonResult('/gen_keys' + query, {method: 'POST'})
      .then(function(res){
        var j = res.json;
        if(!res.ok){
          var msg = (j && j.reason) ? j.reason : ('HTTP ' + res.status);
          toast(msg, 'err', 'key');
          return;
        }
        if(!j || typeof j.result !== 'boolean') throw new Error('invalid key response');
        if(!j.result){ toast(j.reason || 'Key generation failed', 'err', 'key'); return; }
        toast('New key generated · re-pair with the vehicle', 'ok', 'key'); poll();
      })
      .catch(function(){ toast('Key generation failed', 'err', 'key'); });
  });
}

/* ---------- OTA ---------- */
// Status shows inline in the header (progress ring + text) and mirrors into the Firmware pane's
// bar — no bottom popup. A tiny ring sized for the header's status line.
var OTA_CHECK_TIMEOUT_MS=60000, OTA_UPDATE_TIMEOUT_MS=480000, OTA_HTTP_TIMEOUT_MS=5000;
var otaClearTimer=null;
var otaLastToastMilestone=null;
// var(--ok) so it matches the green signal bars while keeping the exact OTA ring geometry.
function otaMiniRing(pct,indet,col){
  var sz=16,c=sz/2,r=6,sw=2.6,circ=2*Math.PI*r; col=col||'var(--accent)';
  if(indet){
    return '<svg class="otaspin" width="'+sz+'" height="'+sz+'" viewBox="0 0 '+sz+' '+sz+'">'+
      '<circle cx="'+c+'" cy="'+c+'" r="'+r+'" fill="none" stroke="var(--border)" stroke-width="'+sw+'"/>'+
      '<circle cx="'+c+'" cy="'+c+'" r="'+r+'" fill="none" stroke="'+col+'" stroke-width="'+sw+'" stroke-linecap="round" stroke-dasharray="'+(circ*0.3).toFixed(1)+' '+circ.toFixed(1)+'"/>'+
      '</svg>';
  }
  var off=circ*(1-Math.max(0,Math.min(100,pct))/100);
  return '<svg width="'+sz+'" height="'+sz+'" viewBox="0 0 '+sz+' '+sz+'" style="transform:rotate(-90deg)">'+
    '<circle cx="'+c+'" cy="'+c+'" r="'+r+'" fill="none" stroke="var(--border)" stroke-width="'+sw+'"/>'+
    '<circle cx="'+c+'" cy="'+c+'" r="'+r+'" fill="none" stroke="'+col+'" stroke-width="'+sw+'" stroke-linecap="round" stroke-dasharray="'+circ.toFixed(1)+'" stroke-dashoffset="'+off.toFixed(1)+'" style="transition:stroke-dashoffset .5s ease"/>'+
    '</svg>';
}
// pct drives the Firmware pane's progress bar: a number (0–100), 'indet' for a phase with no
// measurable progress, or omitted to hide the bar.
function otaInline(html,cls,pct){
  var el=$("otaStat"); if(!el)return;
  el.innerHTML=html||''; el.className='otastat'+(cls?' '+cls:'');
  var bar=$("otaBar"), fill=$("otaFill");
  if(bar&&bar.classList){ bar.classList.toggle('hide',pct==null); bar.classList.toggle('indet',pct==='indet'); }
  if(fill&&fill.style&&pct!=null&&pct!=='indet') fill.style.width=Math.max(0,Math.min(100,pct))+'%';
}
function otaInlineClear(delay){ clearTimeout(otaClearTimer); otaClearTimer=setTimeout(function(){ otaClearTimer=null; otaInline(''); }, delay||3000); }
function otaBegin(phase,timeout){ clearTimeout(otaClearTimer); otaClearTimer=null; otaPhase=phase; otaDeadline=Date.now()+timeout; otaLastToastMilestone=null; renderVerLink(); }
function otaReset(){ clearTimeout(otaTimer); clearTimeout(otaClearTimer); otaClearTimer=null; otaBusy=false; otaPrompting=false; otaPhase=null; otaPollState=null; otaDeadline=0; otaExpectedVersion=null; otaLastToastMilestone=null; renderVerLink(); if(typeof syncOtaUi === 'function') syncOtaUi(); fwRefocus(); }
function otaFail(message){ otaReset(); if(typeof closeOtaModal === 'function') closeOtaModal(false); toast(message, 'err', 'ota'); otaInline('<span>'+esc(message)+'</span>','err'); otaInlineClear(6000); if(typeof syncOtaUi === 'function') syncOtaUi(); }
function otaSchedule(fn,delay){
  if(!otaDeadline||Date.now()<otaDeadline){ otaTimer=setTimeout(fn,delay); return; }
  otaFail(otaPhase==='check'?'check timed out':'update timed out');
}
function otaVersion(v){return typeof v==='string'&&v.length<=31&&/^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z.-]+)?$/.test(v)}


function otaStatus(){
  return requestJsonWithTimeout('/ota/status',{cache:'no-store'},OTA_HTTP_TIMEOUT_MS).then(function(o){
    var valid=o&&['idle','checking','downloading','done','error'].indexOf(o.state)>=0&&
      typeof o.update_available==='boolean'&&typeof o.progress==='number'&&isFinite(o.progress)&&o.progress>=0&&o.progress<=100&&
      typeof o.message==='string'&&typeof o.available==='string'&&typeof o.current==='string';
    if(valid&&(o.update_available||o.state==='downloading'||o.state==='done')) valid=otaVersion(o.available);
    if(!valid) throw new Error('invalid OTA status response');
    return o;
  });
}

/* ---------- update channel (modal selector and Firmware row status) ---------- */
function getActiveChannel(){
  return otaChannel || 'release';
}

function openChannelSelect(){
  if(isOtaFlowBusy()) return;
  var m = $("chanModal");
  if(m && m.classList) m.classList.remove('hide');
  syncModal();
  var card = m ? (m.querySelector ? m.querySelector('.modal-card') : null) : null;
  if(card && typeof card.focus === 'function') card.focus();
}

function closeChannelModal(){
  var m = $("chanModal");
  if(m && m.classList) m.classList.add('hide');
  syncModal();
  var vl = $("verLink");
  if(vl && typeof vl.focus === 'function' && !isOpen('askModal') && !isOpen('otaModal')) vl.focus({ preventScroll: true });
}

function selectChannel(chan){
  closeChannelModal();
  if(chan !== 'release' && chan !== 'dev') return Promise.resolve();
  if(chan === getActiveChannel()) return Promise.resolve();
  fwFocusId = 'verLink';
  return setChannel(chan);
}

function renderChanMenu(){
  var chan = otaChannel || 'release';
  var isDev = (chan === 'dev');
  var chanLabel = isDev ? 'Development' : 'Release';
  var fs = $("fwSub");
  if(fs) fs.textContent = chanLabel + ' channel';
  var br = $("chanBadgeRelease"); if(br && br.classList) br.classList.toggle('hide', isDev);
  var bd = $("chanBadgeDev"); if(bd && bd.classList) bd.classList.toggle('hide', !isDev);
  var or = $("chanOptRelease"); if(or){ if(or.classList) or.classList.toggle('selected', !isDev); or.setAttribute('aria-pressed', isDev ? 'false' : 'true'); }
  var od = $("chanOptDev"); if(od){ if(od.classList) od.classList.toggle('selected', isDev); od.setAttribute('aria-pressed', isDev ? 'true' : 'false'); }
  renderFwVer();
  renderIpLine();
  renderVerLink();
  if(typeof syncOtaUi === 'function') syncOtaUi();
}

function setChannel(chan){
  if(isOtaFlowBusy()) return Promise.resolve();
  if(chan!=='release'&&chan!=='dev') return Promise.resolve();
  var prevChan = otaChannel;
  var changed = (otaChannel !== chan);
  if(!changed) return Promise.resolve();

  otaChannel = chan;
  otaChannelTarget = chan;
  otaChannelInFlight = true;
  otaChannelInitialSet = true;
  renderChanMenu();

  return requestJsonResultWithTimeout('/set_ota', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ channel: chan })
  }, OTA_HTTP_TIMEOUT_MS).then(function(res){
    if(!res.ok){
      var rj = res.json && res.json.response;
      var err = new Error('HTTP ' + res.status);
      err.http = true;   // the device answered and refused
      err.reason = (rj && typeof rj.reason === 'string') ? rj.reason : '';
      throw err;
    }
    otaChannelInFlight = false;
    var label = (chan === 'dev' ? 'Development' : 'Release');
    toast('Update channel set to ' + label, 'ok', 'channel');
    renderChanMenu();
    return otaCheck();
  }).catch(function(err){
    otaChannelInFlight = false;
    otaChannelTarget = null;
    otaChannel = prevChan;
    renderChanMenu();
    if(err && err.http){
      toast('Failed to set update channel' + (err.reason ? ' — ' + err.reason : ''), 'err', 'channel');
    } else {
      // timeout or transport error: the device may or may not have stored the channel, so take its answer
      toast('Update channel change not confirmed — refreshing status', 'err', 'channel');
      poll();
    }
    fwRefocus();
  });
}

var otaDecisionResolve = null;

function closeOtaModal(decision){
  var m = $("otaModal");
  var open = (m && m.classList && typeof m.classList.contains === 'function' && !m.classList.contains('hide'));
  if(!open && !otaDecisionResolve) return;
  if(m && m.classList) m.classList.add('hide');
  syncModal();
  var btn = $("fwCheckBtn") || $("verLink");
  if(btn && typeof btn.focus === 'function' && !isOpen('askModal') && !isOpen('chanModal') && !isOtaBusy()) btn.focus({ preventScroll: true });
  if(otaDecisionResolve){
    var r = otaDecisionResolve;
    otaDecisionResolve = null;
    r(decision === true);
  }
}

function openFwUpdate(){
  if(isOtaFlowBusy()) return Promise.resolve();
  var curVer = (state && state.version) || '?';
  if(otaAvail){
    var ver = otaAvail;                 // the prompt outlives any later change of the global
    otaPrompting = true; fwFocusId = 'fwCheckBtn';
    if(typeof syncOtaUi === 'function') syncOtaUi();
    return loadOtaChangelog().then(function(notes){
      return askOtaInstall({ current: curVer, available: ver }, notes);
    }).then(function(installed){
      if(installed){
        return startOtaUpdate(ver);
      } else {
        otaAvail = null;
        if(state) render(state);
        otaReset();
      }
    }).catch(function(){ otaReset(); });
  }
  return otaCheck();
}

function startOtaUpdate(availVer){
  otaPrompting = false;
  var om = $("otaModal"); if(om && om.classList) om.classList.add('hide');
  var am = $("askModal"); if(am && am.classList) am.classList.add('hide');
  if(typeof askClose === 'function' && askResolve) askClose(null);
  syncModal();
  var ver = availVer || otaAvail;
  otaExpectedVersion = ver || null;
  otaBusy = true;
  otaBegin('update', OTA_UPDATE_TIMEOUT_MS);
  toast('Starting update…', 'load', 'ota');
  otaInline(otaMiniRing(0, true, 'currentColor') + '<span>starting…</span>', '', 'indet');
  if(typeof syncOtaUi === 'function') syncOtaUi();
  return requestJsonWithTimeout('/ota/update', { method: 'POST' }, OTA_HTTP_TIMEOUT_MS).then(function(j){
    if(!j || j.result !== true){
      var reason = (j && j.reason) || 'update did not start';
      throw new Error(reason);
    }
    otaPoll();
  }).catch(function(err){
    otaFail((err && err.message) || 'update failed to start');
  });
}

function loadOtaChangelog(){
  var ctl = typeof AbortController !== 'undefined' ? new AbortController() : null;
  var timer = setTimeout(function(){ if(ctl) ctl.abort(); }, OTA_HTTP_TIMEOUT_MS);
  return fetch('/ota/changelog', {cache: 'no-store', signal: ctl ? ctl.signal : undefined})
    .then(function(r){
      clearTimeout(timer);
      if(!r || !r.ok || r.status === 204) return '';
      return r.text();
    })
    .catch(function(){
      clearTimeout(timer);
      return '';
    });
}

function askOtaInstall(status, changelog){
  if(otaDecisionResolve) closeOtaModal(false);
  var title = $("otaModalTitle");
  if(title) title.textContent = 'Firmware update';
  var verLine = $("otaVersionLine");
  if(verLine) verLine.textContent = 'v' + (status.current || '?') + ' → v' + (status.available || '?');
  var chanEl = $("otaChannel");
  if(chanEl){
    chanEl.textContent = (status.channel === 'dev' || otaChannel === 'dev') ? 'Development' : 'Release';
  }
  var list = $("otaChanges");
  var count = 0;
  if(list){
    list.textContent = '';
    var notes = String(changelog || '').split(/\r?\n/).map(function(s){ return s.trim(); }).filter(Boolean);
    count = notes.length;
    for(var i = 0; i < count; i++){
      var item = document.createElement('li');
      item.textContent = notes[i];
      list.appendChild(item);
    }
    list.hidden = (count === 0);
  }
  var noChanges = $("otaNoChanges");
  if(noChanges){
    noChanges.textContent = 'No changelog was supplied for this update.';
    noChanges.hidden = (count > 0);
  }
  var installBtn = $("otaInstall");
  if(installBtn){
    installBtn.classList.remove('hide');
    installBtn.disabled = false;
  }
  var m = $("otaModal");
  if(m && m.classList) m.classList.remove('hide');
  syncModal();
  var card = m && m.querySelector ? m.querySelector('.modal-card') : null;
  if(card && typeof card.focus === 'function' && !isOpen('askModal')) card.focus();
  return new Promise(function(resolve){
    otaDecisionResolve = resolve;
  });
}

if(typeof document!=='undefined' && typeof document.addEventListener==='function'){
  wireModalFieldSelection(document);
  document.addEventListener('click', function(e){
    var modalBackdrop = $("otaBackdrop");
    if(modalBackdrop && e.target === modalBackdrop){
      closeOtaModal(false);
    }
    var chanBackdrop = $("chanBackdrop");
    if(chanBackdrop && e.target === chanBackdrop){
      closeChannelModal();
    }
    var askBackdrop = $("askBackdrop");
    if(askBackdrop && e.target === askBackdrop){
      askClose(askCancelValue);
    }
  });
  document.addEventListener('keydown', function(e){
    var am = $("askModal");
    var askOpenNow = am && am.classList && typeof am.classList.contains==='function' && !am.classList.contains('hide');
    var cm = $("chanModal");
    var chanOpenNow = cm && cm.classList && typeof cm.classList.contains==='function' && !cm.classList.contains('hide');
    if(e.key==='Escape'){
      if(askOpenNow){ askClose(askCancelValue); return; }
      if(chanOpenNow){ closeChannelModal(); return; }
      var modal = $("otaModal");
      if(modal && modal.classList && typeof modal.classList.contains==='function' && !modal.classList.contains('hide')){
        closeOtaModal(false);
      }
    } else if(e.key==='Enter' && askOpenNow && e.target === $("askInput")){
      e.preventDefault();
      askSubmit();
    }
  });
  var cancelBtn = $("otaCancel");
  if(cancelBtn) cancelBtn.onclick = function(){ closeOtaModal(false); };
  var installBtn = $("otaInstall");
  if(installBtn) installBtn.onclick = function(){ closeOtaModal(true); };
  var askCancelBtn = $("askCancel");
  if(askCancelBtn) askCancelBtn.onclick = function(){ askClose(askCancelValue); };
  var askOkBtn = $("askOk");
  if(askOkBtn) askOkBtn.onclick = askSubmit;
  var askIn = $("askInput");
  if(askIn) askIn.oninput = askValidate;
}

function otaCheck(){
  if(isOtaFlowBusy()) return Promise.resolve();          // a check/update/prompt is already running
  if(!fwFocusId) fwFocusId = 'fwCheckBtn';
  otaBusy=true;
  otaBegin('check',OTA_CHECK_TIMEOUT_MS);
  if(typeof syncOtaUi === 'function') syncOtaUi();
  toast('Checking for updates…', 'load', 'ota');
  otaInline(otaMiniRing(0,true,'currentColor'),'','indet');   // checking — spinning ring only, no label
  var checkUrl='/ota/check?ms='+Date.now();
  return requestJsonWithTimeout(checkUrl,{},OTA_HTTP_TIMEOUT_MS).then(function(j){
    if(!j||j.started!==true) throw new Error((j&&j.reason)||'check did not start');
    return otaCheckPoll();
  }).catch(function(){ otaFail('check failed'); });
}
function otaCheckPoll(){
  clearTimeout(otaTimer);
  return otaStatus().then(function(o){
    if(o.state==='checking'){ otaSchedule(otaCheckPoll,1200); return; }
    if(o.state==='downloading'||o.state==='done'){
      otaExpectedVersion=o.available||null; otaBegin('update',OTA_UPDATE_TIMEOUT_MS); otaProgress(o); return;
    }
    if(o.state==='error'){ otaFail('check failed'+(o.message?' — '+o.message:'')); return; }
    if(o.state!=='idle'){ otaFail('invalid check state'); return; }
    if(o.update_available){
      otaBusy=false;
      otaPhase=null;
      otaPrompting=true;          // stay locked while the changelog loads and the dialog is open
      if(typeof syncOtaUi === 'function') syncOtaUi();
      otaAvail=o.available||''; if(state)render(state); otaInline('');         // clear while the dialog is up
      toast('Update available: v' + o.available, 'ok', 'ota');
      return loadOtaChangelog().then(function(notes){
        return askOtaInstall(o, notes);
      }).then(function(installed){
        if(installed){
          return startOtaUpdate(o.available);
        } else {
          otaAvail=null; if(state)render(state);
          otaReset();
        }
      });
    } else {
      otaAvail=null; if(state)render(state); otaReset();
      if(typeof closeOtaModal === 'function') closeOtaModal(false);
      toast('Firmware is up to date', 'ok', 'ota');
      otaInline('<span>up to date</span>'); otaInlineClear(3500);
    }
  }).catch(function(){ otaFail('check failed'); });
}
function otaPoll(){
  clearTimeout(otaTimer);
  if(otaDeadline&&Date.now()>=otaDeadline){ otaFail('update timed out'); return; }
  return otaStatus().then(otaProgress).catch(function(){ otaSchedule(otaPoll,1500); });
}
function otaProgress(o){
  otaBusy=true;
  otaPollState=o;
  if(typeof syncOtaUi === 'function') syncOtaUi();
  if(o.state==='downloading'){
    var p=Math.min(100, Math.max(0, num(o.progress)||0));
    var milestone=Math.floor(p/10)*10;
    if(otaLastToastMilestone===null || milestone>otaLastToastMilestone){
      otaLastToastMilestone=milestone;
      toast('Downloading update… ' + milestone + '%', 'load', 'ota');
    }
    otaInline(otaMiniRing(p,false,'currentColor')+'<span>'+p+'%</span>','',p);
    otaSchedule(otaPoll,800);
  }
  else if(o.state==='done'){
    if(!otaVersion(otaExpectedVersion)){ otaFail('update target version is missing'); return; }
    clearTimeout(otaTimer); otaPhase='reboot';
    toast('Verifying & rebooting…', 'load', 'ota');
    otaInline(otaMiniRing(100,false,'currentColor')+'<span>verifying…</span>','',100);
    setTimeout(function(){waitReboot(otaExpectedVersion)},1200);
  }
  else if(o.state==='error'){
    if(typeof closeOtaModal === 'function') closeOtaModal(false);
    otaFail('update failed'+(o.message?' — '+o.message:''));
  }
  else if(o.state==='idle'&&otaPhase==='update'){
    // The 1.2 s "done" state can be missed in a background tab. Verify the target version instead
    // of polling a fresh boot's idle state forever or treating idle as success.
    if(!otaVersion(otaExpectedVersion)){ otaFail('update target version is missing'); return; }
    otaPhase='reboot';
    toast('Verifying & rebooting…', 'load', 'ota');
    otaInline(otaMiniRing(100,false,'currentColor')+'<span>verifying…</span>','',100);
    waitReboot(otaExpectedVersion);
  }
  else if(o.state==='checking'){ otaSchedule(otaPoll,1000); }
  else { otaFail('update entered an unexpected state'); }
}
function resumeOta(){
  return otaStatus().then(function(o){
    if(o&&(o.state==='downloading'||o.state==='done')){
      otaBusy=true; otaExpectedVersion=o.available||null; otaBegin('update',OTA_UPDATE_TIMEOUT_MS); otaProgress(o);
    }
  }).catch(function(){});
}
// After "done" the device reboots ~1.2s later. Poll /status in the background and
// reload only after /status reports the exact target version. We can't rely on seeing the device
// go down — by the time polling starts it may already be online again — and a different version is
// not success. Per-request and overall deadlines keep an unreachable device from wedging the UI.
function waitReboot(expectedVer){
  if(!otaVersion(expectedVer)){ otaFail('update target version is missing'); return; }
  var started=Date.now();
  toast('Rebooting… waiting for device', 'load', 'ota');
  (function probe(){
    requestJsonWithTimeout('/status?ms='+Date.now(),{cache:'no-store'},3000)
      .then(function(o){
        if(!o||typeof o.version!=='string') throw new Error('invalid status response');
        if(o.version===expectedVer){
          toast('Updated to v' + expectedVer, 'ok', 'ota');
          setTimeout(function(){ location.reload(); }, 1200);
          return;
        }
        next();
      })
      .catch(next);
    function next(){
      if(Date.now()-started>90000){ otaFail('update rebooted but the expected version was not verified'); return; }
      setTimeout(probe,1000);
    }
  })();
}


function syncOtaUi(){
  var busy = isOtaBusy();
  var running = isOtaRunning();
  var verBtn = $("verLink");
  if(verBtn){
    verBtn.disabled = busy;
    if(verBtn.classList) verBtn.classList.toggle('disabled', busy);
  }
  var fwBtn = $("fwCheckBtn");
  if(fwBtn){
    fwBtn.disabled = busy;
    if(fwBtn.classList) fwBtn.classList.toggle('disabled', busy);
  }
  var paneSettings = $("paneSettings");
  if(paneSettings && paneSettings.classList) paneSettings.classList.toggle('ota-busy', busy);
  var instBtn = $("otaInstall"); if(instBtn && running) instBtn.disabled = true;
  var vb = $("vinBtn"); if(vb) vb.disabled = busy;
  var kb = $("keyBtn"); if(kb) kb.disabled = busy;
  var mb = $("mqttBtn"); if(mb) mb.disabled = busy;
  var sb = $("syslogBtn"); if(sb) sb.disabled = busy;
  if(paneSettings && typeof paneSettings.querySelectorAll === 'function'){
    var rowBtns = paneSettings.querySelectorAll('.row button');
    for(var i = 0; i < rowBtns.length; i++) rowBtns[i].disabled = busy;
  }
  if(busy){
    if(typeof isOpen === 'function' && isOpen('chanModal')){
      closeChannelModal();
    }
    if(running){
      if(typeof isOpen === 'function' && isOpen('otaModal')){
        var om = $("otaModal");
        if(om && om.classList) om.classList.add('hide');
        syncModal();
        if(otaDecisionResolve){
          var r = otaDecisionResolve;
          otaDecisionResolve = null;
          r(false);
        }
      }
      if(typeof isOpen === 'function' && isOpen('askModal')){
        if(typeof askClose === 'function' && askResolve) askClose(null);
      }
    }
  }
}
// Disabling the control that started a check or channel change drops keyboard focus to <body>.
// Called where a flow ends (otaReset, a refused channel change): with the UI idle and no dialog up,
// put focus back where the user was.
function fwRefocus(){
  var id = fwFocusId;
  if(!id || isOtaFlowBusy() || isOpen('askModal') || isOpen('otaModal') || isOpen('chanModal')) return;
  fwFocusId = null;
  var ae = (typeof document !== 'undefined') ? document.activeElement : null;
  var lost = !ae || ae === document.body || ae === document.documentElement || ae.disabled || ae.id === id;
  var el = $(id);
  if(lost && el && typeof el.focus === 'function') el.focus({ preventScroll: true });
}
if(typeof window!=='undefined'){
  window.isOtaRunning = isOtaRunning;
  window.isOtaBusy = isOtaBusy;
  window.syncOtaUi = syncOtaUi;
  window.openFwUpdate = openFwUpdate;
  window.openChannelSelect = openChannelSelect;
  window.closeChannelModal = closeChannelModal;
  window.selectChannel = selectChannel;
  window.setChannel = setChannel;
  window.startOtaUpdate = startOtaUpdate;
  window.askValidate = askValidate;
  window.askOtaInstall = askOtaInstall;
  window.closeOtaModal = closeOtaModal;
  window.otaCheck = otaCheck;
  window.getActiveChannel = getActiveChannel;
}

/* ---------- boot ---------- */
function boot(){
  fetch('/set_time',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ms:Date.now()})}).catch(function(){});
  poll(); setInterval(poll,4000);
  setInterval(function(){ if(feedOk) paintCd(); },1000);
  resumeOta();
}
if(!(typeof window!=='undefined'&&window.__TESLA_UI_NO_BOOT__)) boot();
