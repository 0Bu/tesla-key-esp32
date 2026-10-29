"use strict";
var $=function(id){return document.getElementById(id)};
// Preserve DOM nodes and animation phase when polled markup is unchanged.
function setHTML(el,html){ if(el && el.__h!==html){ el.__h=html; el.innerHTML=html; } }
var state=null, otaTimer=null, otaAvail=null, waking=false, wakeTimeout=null, chgBusy=false, feedOk=false;
var otaBusy=false, otaPhase=null, otaPollState=null, otaDeadline=0, otaExpectedVersion=null, otaChannel='release', otaChannelInitialSet=false;
var otaChannelInFlight=false, otaChannelTarget=null, otaPrompting=false;
var otaFlowToken=null;
var fwFocusId=null;   // control that started the running check/channel change; refocused once the UI is idle again

function isOtaRunning(){
  if(otaPhase === 'update' || otaPhase === 'reboot') return true;
  var st = (typeof otaPollState === 'string') ? otaPollState : (otaPollState && otaPollState.state);
  return st === 'downloading' || st === 'done';
}
function isOtaBusy(){
  return !!(otaBusy || isOtaRunning() || otaChannelInFlight || otaPhase === 'check');
}
// Serialize firmware flows through the update dialog's answer.
function isOtaFlowBusy(){ return isOtaBusy() || otaPrompting; }

// Quotes are escaped too so esc() is safe in attribute values (title="…"), not just element content.
function esc(s){return String(s).replace(/[&<>"']/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]})}
// Admit finite numeric fields before inserting radio data into HTML.
function num(x){ if(typeof x!=='number' && typeof x!=='string') return null; if(typeof x==='string' && !x.trim()) return null; x=+x; return isFinite(x)?x:null; }   // absent/null/'' stay absent (+null is 0)

function requestJson(url,options){
  var opts=Object.assign({},options||{});
  opts.headers=Object.assign({'X-Requested-With':'XMLHttpRequest'},opts.headers||{});
  return fetch(url,opts).then(function(r){
    if(!r||!r.ok){ var e=new Error('HTTP '+(r&&r.status!=null?r.status:'error')); e.status=r&&r.status; throw e; }
    return r.json();
  });
}
function requestJsonResult(url,options){
  var opts=Object.assign({},options||{});
  opts.headers=Object.assign({'X-Requested-With':'XMLHttpRequest'},opts.headers||{});
  return fetch(url,opts).then(function(r){
    return r.json().catch(function(){ return null; }).then(function(j){
      return {ok:!!(r&&r.ok), status:r?r.status:0, json:j};
    });
  });
}
// Bound the entire request, including its body; abort and reject on expiry.
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
// Preserve the device's request-local outcome without inventing physical completion or retrying.
function degradedReply(j){
  var o=j&&j.response?j.response:j;
  return o&&o.reply_degraded===true?o:null;
}
function showDegradedReply(j,key){
  var o=degradedReply(j);
  if(!o) return false;
  toast(o.reason||'Operation outcome unknown; refresh status before another attempt',
        o.outcome==='succeeded'?'info':'err',key);
  poll();
  return true;
}
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
   Keep the last received frame on transport failure, but label it as unavailable. One bounded
   request is shared by interval and action refreshes so a stalled device cannot accumulate polls.
   waitReboot() has a separate post-OTA reboot detection path. */
var statusPoll=null;
function poll(){
  if(statusPoll) return statusPoll;
  // OTA has its own small progress response. Avoid a second full status tree during TLS,
  // and retain the existing in-flight promise so starts can wait for its complete body.
  if(isOtaFlowBusy()) return Promise.resolve();
  statusPoll=requestJsonWithTimeout('/status?ms='+Date.now(),{cache:'no-store'},8000)
    .then(function(s){
      render(s); feedOk=true;
      var status=$("feedStatus"); if(status) status.classList.add('hide');
      settleWakeToast();
    })
    .catch(function(){
      feedOk=false;
      var status=$("feedStatus");
      if(status){
        status.textContent=state?'Device unavailable. Showing last received values.':'Device unavailable. Reconnecting…';
        status.classList.remove('hide');
      }
    })
    .then(function(){ statusPoll=null; });
  return statusPoll;
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
/* WEB_PRESENTERS_BEGIN */
// Mirror the native display/LED SoC ramp and display RSSI thresholds; the host parity gate
// checks all integer SoC values and the signed-byte RSSI range against the actual C++ helpers.
function signalBars(rssi){ return rssi>=-55?4:rssi>=-65?3:rssi>=-75?2:rssi>=-85?1:0; }
function socColor(soc){
  var f=Math.fround, p=soc<=0?0:soc>=100?1:f(soc/100);
  var stops=[[0,231,76,60],[f(0.18),240,190,40],[f(0.45),120,200,90],[f(0.80),60,175,80],[1,30,140,60]];
  for(var i=0;i<stops.length-1;i++){
    var a=stops[i],b=stops[i+1];
    if(p<=b[0]){
      var span=f(b[0]-a[0]),t=span<=0?0:f(f(p-a[0])/span);
      // C++ lroundf rounds half away from zero; preserve its float32 intermediate steps.
      var ch=function(j){var d=f((b[j]-a[j])*t);return a[j]+(d<0?-Math.round(-d):Math.round(d));};
      return 'rgb('+ch(1)+','+ch(2)+','+ch(3)+')';
    }
  }
  return 'rgb(30,140,60)';
}
function barsHTML(rssi){
  // Missing RSSI keeps the faded signal placeholder stable.
  rssi=num(rssi);
  var n = rssi==null ? 0 : signalBars(rssi);
  var H=[4,7,10,13], s='<svg class="bars" width="24" height="15" viewBox="0 0 21 13" fill="currentColor"><title>'+(rssi==null?'No signal':rssi+' dBm')+'</title>';
  for(var i=0;i<4;i++) s+='<rect x="'+(i*5.3)+'" y="'+(13-H[i])+'" width="3.3" height="'+H[i]+'" rx="1" opacity="'+(i<n?1:0.25)+'"/>';
  return s+'</svg>';
}
/* WEB_PRESENTERS_END */
// Animated signal bars share RSSI geometry; CSS owns their phase.
function searchBarsHTML(warm){
  var H=[4,7,10,13], s='<svg class="bars search'+(warm?' warm':'')+'" width="24" height="15" viewBox="0 0 21 13" fill="currentColor"><title>Searching…</title>';
  for(var i=0;i<4;i++) s+='<rect class="sb'+(i+1)+'" x="'+(i*5.3)+'" y="'+(13-H[i])+'" width="3.3" height="'+H[i]+'" rx="1"/>';
  return s+'</svg>';
}
// Outline bars distinguish a disconnected radio from a faded reading.
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
// data-p binds a row to exactly one countdown; only its text changes each tick.
function cdHTML(kind){ return '<span class="cd" data-p="'+kind+'"></span>'; }
// Resync on a new countdown or a discrepancy of at least two seconds.
function cdSync(kind,secs){
  if(kind!=='givesup'&&kind!=='retries'){ cdKind=null; cdEndMs=0; return; }
  if(secs==null) secs=0;
  if(kind!==cdKind||Math.abs(cdLeft()-secs)>=2){ cdKind=kind; cdEndMs=Date.now()+secs*1000; }
}
function cdLeft(){ return Math.max(0,Math.ceil((cdEndMs-Date.now())/1000)); }
// Paint only the matching countdown; at zero, describe the pending phase transition.
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
// Browser mirror of logic/ble_row.hpp; parity tests pin its row decisions.
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
/* ---------- icons (2px outline shapes; stroke styling lives in CSS) ---------- */
var ICON={
  bolt:'<use href="#ic-bolt"/>',
  warn:'<use href="#ic-warn"/>',
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
/* Climate has no per-domain age in /status. Show explicitly cached settings/states, never
   infer current activity from another domain's last_seen_s or attribute charger input to a
   climate function. Raw charger_phases also does not establish the AC topology multiplier. */
function copChip(s){
  var c=s&&s.tele&&s.tele.climate;
  if(!c||(c.cop!=='On'&&c.cop!=='FanOnly')) return '';
  var tip='Last reported cabin overheat protection setting; current activity and power are unknown'+
          (c.cop_temp?' · '+c.cop_temp:'');
  return '<div class="stat cached" title="'+esc(tip)+'"><div class="k"><span>Overheat setting</span></div>'+
         '<div class="v">'+(c.cop==='FanOnly'?'Fan only':'On')+'</div></div>';
}
function defrostChip(s){
  var c=s&&s.tele&&s.tele.climate;
  if(!c) return '';
  var p=[]; if(c.front_defrost===true)p.push('Front'); if(c.rear_defrost===true)p.push('Rear');
  if(c.defrost_mode==='Normal'||c.defrost_mode==='Max')p.push(c.defrost_mode);
  if(!p.length) return '';
  var tip='Last reported defrost state; current activity and power are unknown';
  return '<div class="stat cached" title="'+tip+'"><div class="k"><span>Defrost (cached)</span></div>'+
         '<div class="v">'+p.join(' / ')+'</div></div>';
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

// Only the named gauge button invokes the primary action. The surrounding card is informational.
var heroActFn=null;
function heroTap(){ if(heroActFn) return heroActFn(); }
function setHeroAct(label,icon,fn,busyText){ heroActFn=busyText?null:fn; }

/* ---------- render ---------- */
// Connection row: tone ('ok' | 'warn' | '') colours the icon dot, status line and signal glyph.
function setRow(key,tone,valueHTML,statusHTML,barsMarkup){
  var d=$(key+'Dot'); if(d) d.className='dot'+(tone?' '+tone:'');
  setHTML($(key+'Val'),valueHTML);
  var st=$(key+'St'); if(st){ st.className='rs'+(tone&&tone!=='err'?' '+tone:''); setHTML(st,statusHTML); }
  var b=$(key+'Bars'); if(b){ b.className='rbar'+(tone?' '+tone:''); setHTML(b,barsMarkup||''); }
}
function lastChips(s){
  var ls=s.last||{}, lsoc=num(ls.usable_soc), chips=[];
  if(lsoc==null) lsoc=num(ls.soc);
  if(lsoc!=null) lsoc=Math.round(Math.max(0,Math.min(100,lsoc)));
  var ago=fmtAgo(num(s.last_seen_s));
  if(lsoc!=null) chips.push(stat('Last battery','<span style="color:'+socColor(lsoc)+'">'+lsoc+'</span>','%'));
  var chargeAgo=fmtAgo(num(s.charge_age_s));
  if(chargeAgo)  chips.push(stat('Updated',chargeAgo,''));
  if(ago && s.link!=='awake') chips.push(stat('Idle', ago, ''));
  return {html:chips.join(''), soc:lsoc};
}
function render(s){
  state=s;
  var linked=!!(s.ble&&s.ble.connected), paired=!!s.paired||!!s.session_available;
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

  // Fatal/identity recovery is distinct from normal automatic pairing; no manual key action.
  var rb=$("safeModeBanner"), runtime=s.sys&&s.sys.runtime_state, identity=s.identity_state;
  var recoveryText='';
  if(runtime==='fatal'){
    recoveryText='<b>Device recovery required</b> — '+esc(s.sys.fatal_component||'unknown')+
      '. Vehicle services are stopped. Retain diagnostics and resolve the cause before restarting.';
  } else if(safe){
    recoveryText='<b>Safe Mode active</b> — Vehicle Bluetooth, commands, and telemetry are stopped. Use this recovery dashboard to inspect diagnostics or update firmware.';
  } else if(identity==='reboot_required'){
    recoveryText='<b>Identity recovery</b> — The saved identity needs to be reloaded. Keep the device powered while automatic recovery is evaluated; inspect diagnostics if this remains.';
  } else if(identity==='cleanup_pending'){
    recoveryText='<b>Identity recovery</b> — Saved-key cleanup is pending and will retry automatically.';
  } else if(identity==='vin_transition'){
    recoveryText='<b>Identity recovery</b> — A vehicle identity change is waiting for recovery. Inspect diagnostics if this remains.';
  } else if(identity==='storage_unavailable'){
    recoveryText='<b>Identity recovery</b> — Key storage is unavailable. The device will check again automatically.';
  }
  rb.classList.toggle('show',!!recoveryText);
  if(recoveryText) setHTML(rb.querySelector('.bt'),recoveryText);
  rb.querySelector('button').hidden=!(safe||runtime==='fatal');

  // setHTML preserves the hero animation phase between unchanged polls.
  var hic=$("hicon"), hl=$("hlabel"), hs=$("hsub"), hst=$("hstats");
  var g={mode:'neutral', pct:null, glyph:null, busy:false, limit:null}, head='', sub='', chips='';
  var act=null, actLabel='', actIcon='bolt', busyText=null;
  $("hero").classList.remove('hide');
  var blocked=s.pairing_blocked;
  if(s.sys && s.sys.runtime_state==='fatal'){
    g={mode:'empty', glyph:'warn'};
    head='Device recovery required'; sub='Vehicle services are stopped. Inspect diagnostics and resolve the fault before restarting the device.';
  } else if(blocked || (identity && identity!=='ok' && identity!=='unknown')){
    g={mode:'empty', glyph:'key'};
    head='Pairing paused';
    sub=blocked==='key_storage_unavailable'
      ? 'Key storage is unavailable. The device will check again automatically.'
      : blocked==='key_cleanup_pending'
      ? 'Saved-key cleanup is pending. The device will retry automatically.'
      : 'The saved identity needs recovery. Keep the device powered; inspect diagnostics if this remains.';
  } else if(configured && paired && s.reauth && !safe){
    g={mode:'neutral', glyph:'link'};
    head='Reconnecting…'; sub='Reconnecting automatically with your existing key.';
  } else if(paired&&v){
    if(waking){ waking=false; clearTimeout(wakeTimeout); }   // car is awake & reporting — stop the spinner
    var soc=(v.usable_soc!=null?num(v.usable_soc):num(v.soc));   // null = the car has not reported a level (never 0 %)
    // "Charge complete" = battery reached its target and not charging; the start tap is gated
    // (the car would only reject a charge_start here as "complete"). See chargeComplete().
    var complete=chargeComplete(v,charging);
    g={mode:charging?'charging':(complete?'complete':'soc'), pct:soc, busy:chgBusy, limit:num(v.charge_limit), glyph:(soc==null?'car':null)};
    head=complete?'Charge complete':(v.status||'Idle');
    if(soc==null) sub='Battery level not reported yet';
    chips=chargeStats(v,charging)+copChip(s)+defrostChip(s);
    if(chgBusy) busyText='Sending command…';
    else if(!complete){ act=toggleCharge; actLabel=charging?'Stop charging':'Start charging'; }
  } else if(paired){
    var lk=lastChips(s);
    if(s.link==='awake'){
      if(waking){ waking=false; clearTimeout(wakeTimeout); }
      g={mode:'neutral', pct:lk.soc, glyph:'car'};
      head='Charge reading unavailable'; sub='Waiting for a fresh charge reading.'; chips=lk.html;
    } else if(waking){
      g={mode:'waking', pct:lk.soc, glyph:'alarm'};
      head='Waking up…'; sub='Reaching your Tesla over Bluetooth…'; busyText='Waking up…';
    } else if(s.link==='asleep'){
      // Proven reachable-but-sleeping: the always-on VCSEC health poll answered recently, so
      // "asleep" is a fact and the wake button is meaningful. Last-known battery + how long the
      // car has been asleep come from the retained cache — shown without waking the car.
      // Keep the asleep card to last-known charge data; climate chips never trigger a poll.
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
    } else if((s.ble&&s.ble.connect_fail)>=BLE_CONNECT_FAIL_WARN){
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
    head='Ready to pair'; sub='Your key is sent automatically. Place your Tesla keycard on the console reader and approve on the touchscreen.';
  } else if((s.ble&&s.ble.connect_fail)>=BLE_CONNECT_FAIL_WARN){
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
  var pairedTxt=s.paired_at?('Session first observed '+new Date(s.paired_at*1000).toLocaleDateString(undefined,{year:'numeric',month:'short',day:'numeric'})):'Paired with the vehicle';
  setSub("vehSub", paired?pairedTxt:(hasVin?'Not paired yet':'Add the VIN to begin'), '');
  var vinTx=hasVin?'Change VIN':'Add VIN';
  var vb=$("vinBtn"); if(vb){ vb.setAttribute('aria-label',vinTx); vb.setAttribute('title',vinTx); }

  // Render the parity-tested BLE presenter; raw scan state carries no deadline.
  var ble=s.ble||{};
  // Presenter input may use usable RAM; the public durable paired field stays unchanged.
  var d=bleRowFromStatus(Object.assign({},s,{paired:paired}));
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
    // The GATT link is up. Stateless (paired but reachability unknown/stale) shows the amber wave
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
  // Paint immediately after a push so replaced countdown nodes are never blank.
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
    if(s.reauth) setSub("keySub",'Existing key preserved — connection needs attention','warn');
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
      if(showDegradedReply(j,'charge')) return;
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
  if(r.indexOf('command outcome unknown')===0)
    return {msg:String(reason), type:'err'};
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
// Make the background inert while a modal is open.
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
  // Desktop focuses/selects input; touch focuses the card to avoid opening the keyboard.
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
        if(showDegradedReply(res.json,'config')) return;
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
      if(utf8Len(v)>64) return 'Broker too long (max 64 bytes)';
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
        if(showDegradedReply(res.json,'config')) return;
        var o=res.json&&res.json.response;
        if(o && typeof o.result === 'boolean' && typeof o.reason === 'string'){
          if(!o.result){ toast(o.reason||'Failed to save MQTT broker', 'err', 'mqtt'); return; }
          if(/no reboot|unchanged|already/i.test(o.reason||'')){ toast(v?'MQTT broker unchanged':'MQTT already disabled', 'info', 'mqtt'); return; }
          toast(o.reason||'Saved · rebooting', 'info', 'mqtt');
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
      if(utf8Len(v)>64) return 'Server too long (max 64 bytes)';
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
        if(showDegradedReply(res.json,'config')) return;
        var o=res.json&&res.json.response;
        if(o && typeof o.result === 'boolean' && typeof o.reason === 'string'){
          if(!o.result){ toast(o.reason||'Failed to save Syslog server', 'err', 'syslog'); return; }
          if(/no reboot|unchanged|already/i.test(o.reason||'')){ toast(v?'Syslog server unchanged':'Syslog already disabled', 'info', 'syslog'); return; }
          toast(o.reason||'Saved · rebooting', 'info', 'syslog');
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
      if(showDegradedReply(j,'wake')){ wakeStop(); return; }
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
        if(showDegradedReply(j,'key')) return;
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
// Action feedback uses the shared bottom popup; the Firmware row keeps its progress bar.
var OTA_CHECK_TIMEOUT_MS=60000, OTA_UPDATE_TIMEOUT_MS=480000, OTA_HTTP_TIMEOUT_MS=5000;
var otaLastToastMilestone=null;
// pct drives the Firmware row's progress bar: a number (0–100), 'indet' for a phase with no
// measurable progress, or omitted to hide the bar.
function otaBarProgress(pct){
  var bar=$("otaBar"), fill=$("otaFill");
  if(bar&&bar.classList){ bar.classList.toggle('hide',pct==null); bar.classList.toggle('indet',pct==='indet'); }
  if(fill&&fill.style&&pct!=null&&pct!=='indet') fill.style.width=Math.max(0,Math.min(100,pct))+'%';
}
function otaBegin(phase,timeout){ otaFlowToken={}; otaPhase=phase; otaDeadline=Date.now()+timeout; otaLastToastMilestone=null; renderVerLink(); }
function otaReset(){ otaFlowToken=null; clearTimeout(otaTimer); otaBarProgress(); otaBusy=false; otaPrompting=false; otaPhase=null; otaPollState=null; otaDeadline=0; otaExpectedVersion=null; otaLastToastMilestone=null; renderVerLink(); if(typeof syncOtaUi === 'function') syncOtaUi(); fwRefocus(); }
function otaAfterStatusPoll(start){
  var token=otaFlowToken;
  return (statusPoll||Promise.resolve()).then(function(){
    if(!token||otaFlowToken!==token) return null;
    if(!otaDeadline||Date.now()>=otaDeadline) throw new Error(otaPhase==='check'?'check timed out':'update timed out');
    return start();
  });
}
function otaFail(message){ otaReset(); if(typeof closeOtaModal === 'function') closeOtaModal(false); toast(message, 'err', 'ota'); if(typeof syncOtaUi === 'function') syncOtaUi(); }
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
    if(degradedReply(res.json)){
      otaChannelInFlight=false; otaChannelTarget=null; otaChannel=prevChan;
      renderChanMenu(); showDegradedReply(res.json,'channel'); fwRefocus(); return;
    }
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
      return askOtaInstall({ current: curVer, available: ver, pr: Number(otaPrSelection()), channel: otaChannel }, notes);
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

// Read refusal details under the same deadline as successful start replies.
function requestOtaStart(url,options){
  return requestJsonResultWithTimeout(url,options,OTA_HTTP_TIMEOUT_MS).then(function(r){
    if(!r.ok){
      var reason=r.json&&typeof r.json.reason==='string'?r.json.reason:'';
      throw new Error(reason||'HTTP '+r.status);
    }
    return r.json;
  });
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
  var token=otaFlowToken;
  toast('Starting update…', 'load', 'ota');
  otaBarProgress('indet');
  if(typeof syncOtaUi === 'function') syncOtaUi();
  var updatePr=otaPrSelection();
  return otaAfterStatusPoll(function(){
    return requestOtaStart('/ota/update'+(updatePr?'?pr='+updatePr:''), { method: 'POST' });
  }).then(function(j){
    if(otaFlowToken!==token) return;
    if(!j || j.result !== true){
      var reason = (j && j.reason) || 'update did not start';
      throw new Error(reason);
    }
    otaPoll();
  }).catch(function(err){
    if(otaFlowToken===token) otaFail((err && err.message) || 'update failed to start');
  });
}

function loadOtaChangelog(){
  // The notes deadline covers headers and body while the OTA prompt remains locked.
  return timedRequest(function(url,opts){
    return fetch(url,opts).then(function(r){
      if(!r || !r.ok || r.status === 204) return '';
      return r.text();
    });
  }, '/ota/changelog', {cache: 'no-store'}, OTA_HTTP_TIMEOUT_MS).catch(function(){ return ''; });
}

function sizeOtaChanges(){
  var list = $("otaChanges");
  if(!list || !isOpen('otaModal')) return;
  var items = list.children;
  list.style.maxHeight = '';
  if(items.length <= 5) return;
  // Use the shortest five-item span so wrapping cannot expose six complete entries elsewhere.
  var height = Infinity;
  for(var i = 4; i < items.length; i++){
    height = Math.min(height, items[i].offsetTop + items[i].offsetHeight - items[i - 4].offsetTop);
  }
  list.style.maxHeight = height + 'px';
}
if(typeof window !== 'undefined' && typeof window.addEventListener === 'function'){
  window.addEventListener('resize', sizeOtaChanges);
}

function askOtaInstall(status, changelog){
  if(otaDecisionResolve) closeOtaModal(false);
  var title = $("otaModalTitle");
  if(title) title.textContent = 'Firmware update';
  var verLine = $("otaVersionLine");
  if(verLine) verLine.textContent = 'v' + (status.current || '?') + ' → v' + (status.available || '?');
  var chanEl = $("otaChannel");
  if(chanEl){
    chanEl.textContent = (status.pr > 0) ? 'PR preview #' + status.pr
      : (status.channel === 'dev' || otaChannel === 'dev') ? 'Development' : 'Release';
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
  sizeOtaChanges();
  var card = m && m.querySelector ? m.querySelector('.modal-card') : null;
  if(card && typeof card.focus === 'function' && !isOpen('askModal')) card.focus();
  if(list) list.scrollTop = 0;
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

// Match tk::parse_pr_query: first pr key only, 1–7 digits, no leading zero. Read per call.
function otaPrSelection(){
  var q=(typeof location!=='undefined'&&typeof location.search==='string')?location.search:'';
  var parts=q.replace(/^\?/,'').split('&');
  for(var i=0;i<parts.length;i++){
    var eq=parts[i].indexOf('=');
    var key=eq<0?parts[i]:parts[i].slice(0,eq);
    if(key==='pr') return /^[1-9][0-9]{0,6}$/.test(eq<0?'':parts[i].slice(eq+1))?parts[i].slice(eq+1):'';
  }
  return '';
}
function otaCheck(){
  if(isOtaFlowBusy()) return Promise.resolve();          // a check/update/prompt is already running
  if(!fwFocusId) fwFocusId = 'fwCheckBtn';
  otaBusy=true;
  otaBegin('check',OTA_CHECK_TIMEOUT_MS);
  var token=otaFlowToken;
  if(typeof syncOtaUi === 'function') syncOtaUi();
  toast('Checking for updates…', 'load', 'ota');
  otaBarProgress('indet');
  var prSel=otaPrSelection();
  return otaAfterStatusPoll(function(){
    var checkUrl='/ota/check?'+(prSel?'pr='+prSel+'&':'')+'ms='+Date.now();
    return requestOtaStart(checkUrl,{});
  }).then(function(j){
    if(otaFlowToken!==token) return;
    if(!j||j.started!==true) throw new Error((j&&j.reason)||'check did not start');
    return otaCheckPoll();
  }).catch(function(err){ if(otaFlowToken===token) otaFail((err && err.message) || 'check failed'); });
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
      otaAvail=o.available||''; if(state)render(state); otaBarProgress();
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
    }
  }).catch(function(err){ otaFail((err && err.message) || 'check failed'); });
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
    otaBarProgress(p);
    otaSchedule(otaPoll,800);
  }
  else if(o.state==='done'){
    if(!otaVersion(otaExpectedVersion)){ otaFail('update target version is missing'); return; }
    clearTimeout(otaTimer); otaPhase='reboot';
    toast('Verifying & rebooting…', 'load', 'ota');
    otaBarProgress(100);
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
    otaBarProgress(100);
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
// After reboot, reload only on the exact target version, within request and overall deadlines.
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
// Restore the initiating control's focus after the OTA flow becomes idle.
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
  requestJson('/set_time',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ms:Date.now()})}).catch(function(){});
  poll(); setInterval(poll,4000);
  setInterval(function(){ if(feedOk) paintCd(); },1000);
  resumeOta();
}
if(!(typeof window!=='undefined'&&window.__TESLA_UI_NO_BOOT__)) boot();
