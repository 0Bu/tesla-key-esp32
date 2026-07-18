"use strict";
var $=function(id){return document.getElementById(id)};
// Write html into el only when it actually changed (cached on el.__h). The ~2 s WS push
// re-renders on every frame; blindly reassigning innerHTML would destroy and recreate
// the child nodes, restarting any CSS animation on them from 0% — making looping
// animations (hero ring, "searching" signal bars, pulsing dot) visibly jump. Skipping
// the write when nothing changed keeps the same nodes alive so the animation runs on.
function setHTML(el,html){ if(el && el.__h!==html){ el.__h=html; el.innerHTML=html; } }
var state=null, otaTimer=null, otaAvail=null, waking=false, wakeTimeout=null, chgBusy=false, ws=null;

function esc(s){return String(s).replace(/[&<>]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;'}[c]})}

/* ---------- toasts ---------- */
function toast(msg,type){
  type=type||'info'; var c=$("toasts"); if(!c)return;
  while(c.children.length>2) c.removeChild(c.firstChild);
  var t=document.createElement('div'); t.className='toast '+type;
  t.innerHTML='<span class="ic">'+(type==='ok'?'✓':type==='err'?'!':'i')+'</span><span>'+esc(msg)+'</span>';
  c.appendChild(t);
  setTimeout(function(){ t.classList.add('leaving'); setTimeout(function(){if(t.parentNode)t.parentNode.removeChild(t)},230); }, type==='err'?5200:3000);
}

/* ---------- net ----------
   The live UI is WebSocket-only. boot() opens ONE socket to /events; the device pushes the /status
   JSON — an immediate snapshot on "sub", then a fresh copy every ~2 s (http_events.cpp). There is
   no interval polling and no HTTP fallback for the live feed. poll() just asks the open socket for
   an immediate snapshot after a user action, so the UI refreshes now instead of on the next push;
   if the socket is momentarily down it's a no-op and the optimistic local state carries the UI
   until it reconnects. (waitReboot() still GETs /status directly — that's post-OTA reboot
   detection, a different concern from the live feed.) */
function poll(){ try{ if(ws && ws.readyState===1) ws.send('sub'); }catch(e){} }

function connectWS(){
  var proto = location.protocol==='https:' ? 'wss:' : 'ws:';
  ws = new WebSocket(proto+'//'+location.host+'/events');
  ws.onopen    = function(){ try{ ws.send('sub'); }catch(e){} };   // ask for the first snapshot
  ws.onmessage = function(e){ try{ render(JSON.parse(e.data)); }catch(err){} };
  ws.onerror   = function(){ try{ ws.close(); }catch(e){} };       // funnel errors through onclose
  // Keep the last-rendered state on screen (matches the old failed-poll behaviour) and reconnect.
  ws.onclose   = function(){ ws=null; setTimeout(connectWS,3000); };
}

/* ---------- signal glyph ---------- */
function barsHTML(rssi){
  // rssi==null → no link (e.g. BLE disconnected): draw the glyph with every bar faded so
  // the row still shows a signal placeholder instead of jumping to text-only.
  var n = rssi==null ? 0 : (rssi>=-55?4 : rssi>=-67?3 : rssi>=-78?2 : rssi>=-90?1:0);
  var H=[4,7,10,13], s='<svg class="bars" width="24" height="15" viewBox="0 0 21 13" fill="currentColor"><title>'+(rssi==null?'No signal':rssi+' dBm')+'</title>';
  for(var i=0;i<4;i++) s+='<rect x="'+(i*5.3)+'" y="'+(13-H[i])+'" width="3.3" height="'+H[i]+'" rx="1" opacity="'+(i<n?1:0.25)+'"/>';
  return s+'</svg>';
}
// animated "searching…" glyph — same geometry as barsHTML, but each bar carries an
// .sbN class whose @keyframes light it green in turn (cumulative fill), looping until
// the BLE link is up. Opacity is driven entirely by CSS, so no opacity attr here.
function searchBarsHTML(){
  var H=[4,7,10,13], s='<svg class="bars search" width="24" height="15" viewBox="0 0 21 13" fill="currentColor"><title>Searching…</title>';
  for(var i=0;i<4;i++) s+='<rect class="sb'+(i+1)+'" x="'+(i*5.3)+'" y="'+(13-H[i])+'" width="3.3" height="'+H[i]+'" rx="1"/>';
  return s+'</svg>';
}
// "status unknown" glyph — same geometry as barsHTML, all four bars fully lit (the link
// is up; signal strength is real and shown as dBm next to it). Each bar carries a .wbN
// class; its own @keyframes (wbpN) fades it between dark and light orange so a light crest
// ping-pongs edge→edge across the bars. The markup is rssi-independent so it stays
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
function ringSVG(pct,dim,col,sz){
  sz=sz||104; col=col||'var(--accent)';
  var c2=sz/2, r=c2-7, circ=2*Math.PI*r, off=circ*(1-pct/100), sw=8;
  return '<svg width="'+sz+'" height="'+sz+'" viewBox="0 0 '+sz+' '+sz+'" style="transform:rotate(-90deg)">'+
    '<circle cx="'+c2+'" cy="'+c2+'" r="'+r+'" fill="none" stroke="var(--soft)" stroke-width="'+sw+'"/>'+
    (dim?'':'<circle cx="'+c2+'" cy="'+c2+'" r="'+r+'" fill="none" stroke="'+col+'" stroke-width="'+sw+'" stroke-linecap="round" stroke-dasharray="'+circ.toFixed(1)+'" stroke-dashoffset="'+off.toFixed(1)+'" style="transition:stroke-dashoffset .6s ease"/>')+
    '</svg>';
}
// charging — SOC arc in a solid colour with an even pulsing glow.
// (A linearGradient stroke colours by geometric position, not along the arc,
//  so on a near-full ring it shows a hard light→dark seam — avoided here.)
function ringCharge(pct,col,sz){
  sz=sz||104; var c2=sz/2, r=c2-7, circ=2*Math.PI*r, sw=8, off=circ*(1-Math.max(0,Math.min(100,pct))/100);
  return '<svg class="ringrot" width="'+sz+'" height="'+sz+'" viewBox="0 0 '+sz+' '+sz+'" style="transform:rotate(-90deg)">'+
    '<circle cx="'+c2+'" cy="'+c2+'" r="'+r+'" fill="none" stroke="var(--soft)" stroke-width="'+sw+'"/>'+
    '<circle cx="'+c2+'" cy="'+c2+'" r="'+r+'" fill="none" stroke="'+col+'" stroke-width="'+sw+'" stroke-linecap="round" stroke-dasharray="'+circ.toFixed(1)+'" stroke-dashoffset="'+off.toFixed(1)+'" style="transition:stroke-dashoffset .6s ease;filter:drop-shadow(0 0 3px '+col+')"/>'+
    '</svg>';
}
// asleep — grey gradient ring with a slow opacity pulse. `fast` (waking) keeps the same
// grey but pulses a little faster via the .waking class — never turns red.
function ringSleep(sz,fast){
  sz=sz||104; var c2=sz/2, r=c2-7, sw=8;
  return '<svg class="ringrot zzz'+(fast?' waking':'')+'" width="'+sz+'" height="'+sz+'" viewBox="0 0 '+sz+' '+sz+'">'+
    '<defs><linearGradient id="sg" x1="0" y1="0" x2="1" y2="1">'+
      '<stop offset="0%" style="stop-color:var(--muted);stop-opacity:.5"/>'+
      '<stop offset="100%" style="stop-color:var(--muted);stop-opacity:.18"/>'+
    '</linearGradient></defs>'+
    '<circle cx="'+c2+'" cy="'+c2+'" r="'+r+'" fill="none" stroke="url(#sg)" stroke-width="'+sw+'"/>'+
    '</svg>';
}
var BOLT='<svg viewBox="0 0 24 24" width="38" height="38" style="fill:var(--muted);opacity:.4"><path d="M13 2 4 14h6l-1 8 9-12h-6l1-8z"/></svg>';
// Bluetooth rune — "can't connect" hero glyph, in warn-orange (same --warn as the "At
// connection limit" row); the ring stays grey via ringSVG(0,true).
var BT='<svg class="glyphbig" viewBox="0 0 24 24" width="40" height="40" fill="none" stroke="var(--warn)" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m7 7 10 10-5 5V2l5 5L7 17"/></svg>';
// Bluetooth rune in grey — neutral hero glyph for the pre-paired setup steps ("Set up needed"
// and "Pairing"): the flow is about the BLE link, not an error (the orange BT above is the
// failure state).
var BTG='<svg class="glyphbig" viewBox="0 0 24 24" width="40" height="40" fill="none" stroke="var(--muted)" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="opacity:.5"><path d="m7 7 10 10-5 5V2l5 5L7 17"/></svg>';
// NFC keycard (card + contactless waves), grey — "Pairing" hero glyph: rest a Tesla NFC
// keycard on the console reader, then approve on the touchscreen (mirrors the hero text).
var CARD='<svg class="glyphbig" viewBox="0 0 24 24" width="44" height="44" fill="none" stroke="var(--muted)" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" style="opacity:.6"><rect x="2" y="7" width="13" height="10" rx="2"/><path d="M2 10.4H15"/><path d="M17.6 9.3a4 4 0 0 1 0 5.4"/><path d="M20 7.6a7 7 0 0 1 0 8.8"/></svg>';
// crescent moon — asleep hero glyph (grey, soft; pairs with the muted ring)
var MOON='<svg class="glyphbig" viewBox="0 0 24 24" width="52" height="52" style="fill:var(--muted);opacity:.72"><path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/></svg>';
// parking "P" — "idle" hero glyph: car is parked & reachable but not provably asleep, so we
// show a neutral "Parked" card (no sleep claim). Matches the Tesla app's wording.
var PARKED='<svg class="glyphbig" viewBox="0 0 24 24" width="48" height="48" fill="none" stroke="var(--muted)" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" style="opacity:.7"><rect x="4" y="4" width="16" height="16" rx="4"/><path d="M9.5 16V8h3.2a2.4 2.4 0 0 1 0 4.8H9.5"/></svg>';
// alarm clock — waking hero glyph (grey outline, gently trembling via .wecker)
var ALARM='<svg class="wecker glyphbig" viewBox="0 0 24 24" width="52" height="52" fill="none" stroke="var(--muted)" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" style="opacity:.82"><circle cx="12" cy="13" r="8"/><path d="M12 9v4l2 2"/><path d="M5 3 2 6"/><path d="m22 6-3-3"/><path d="M6.38 18.7 4 21"/><path d="M17.64 18.67 20 21"/></svg>';
// charge complete — green check shown in the hero label when the battery has reached its
// target (SOC >= charge limit). Inline fill overrides the label's default red (--accent).

/* charging detail chips under the hero label — power + current, shown only
   while the car is actively charging (both read 0 otherwise). */
function stat(k,val,unit,acc){
  return '<div class="stat'+(acc?' acc':'')+'"><div class="k">'+k+'</div><div class="v">'+val+
         (unit?'<small>'+unit+'</small>':'')+'</div></div>';
}
function chargeStats(v,charging){
  if(!v||!charging) return '';
  var c=[];
  if(v.power>0) c.push(stat('Power',   Math.round(v.power), 'kW', true));
  if(v.amps>0)  c.push(stat('Current', Math.round(v.amps),        'A',  true));
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
  return '<div class="stat cop on" title="'+tip+'"><div class="k">Overheat</div>'+
         '<div class="v">'+kw+'</div></div>';
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
  return '<div class="stat defrost on" title="'+tip+'"><div class="k">Defrost</div>'+
         '<div class="v">'+kw+'</div></div>';
}
/* Make every chip in a row the same width — the widest one's content width (so they line
   up tidily without stretching to fill the hero). Reset, measure the max, then pin all. */
function equalizeChips(box){
  if(!box||!box.children.length) return;
  var ch=box.children, i, w=0;
  for(i=0;i<ch.length;i++) ch[i].style.width='';
  for(i=0;i<ch.length;i++) w=Math.max(w, ch[i].offsetWidth);
  for(i=0;i<ch.length;i++) ch[i].style.width=w+'px';
}
// battery at/above its target and not charging → starting a charge is a no-op the car only
// rejects as "complete". charge_limit is emitted by /status only when the car reported it, so
// an unknown limit (field absent) leaves the start button live rather than blocking on a guess.
function chargeComplete(v,charging){
  return !charging && !!v && v.charge_limit!=null && (v.soc!=null?v.soc:0)>=v.charge_limit;
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

/* ---------- render ---------- */
function render(s){
  state=s;
  var linked=!!(s.ble&&s.ble.connected), paired=!!s.paired;
  var hasVin=s.vin&&s.vin!=='UNKNOWN', configured=hasVin&&s.key_present;
  var v=s.vehicle, charging=v&&/charg/i.test(v.status||'');

  // connection — Wi-Fi (signal + SSID + IP), moved here from the header
  var w=s.wifi||{}, wc=$("wifiConn");
  $("wifiLbl").textContent = w.std || 'Wi-Fi';   // standard sits in the label, e.g. "Wi-Fi 4"
  if(w.ssid||s.ip){
    var wbars=(w.rssi!=null)?barsHTML(w.rssi):'';                              // bars (signal glyph)
    var wdbm=(w.rssi!=null)?'<span class="dbm">'+w.rssi+' dBm</span>':'';      // numeric reading
    var wtxt=w.ssid?'<span class="nm">'+esc(w.ssid)+'</span>':'';
    wc.className='cv ok';
    setHTML(wc,wbars+wdbm+wtxt);
  } else {
    // not connected — the bars fill up green one after another (same animation as the BLE
    // "searching" row) instead of a plain text label, while the device keeps reconnecting.
    // setHTML keeps the same bar nodes between polls so the fill animation doesn't restart.
    wc.className='cv'; setHTML(wc,searchBarsHTML()+'<span>Searching…</span>');
  }

  // re-pair notice — key was removed by the car and regenerated
  var rb=$("reauthBanner");
  if(s.reauth && !paired){
    rb.classList.add('show');
    rb.querySelector('.bt').innerHTML='<b>Key was reset.</b> The vehicle removed this device’s key, so a fresh one was generated automatically. Approve the new pairing on your Tesla’s touchscreen.';
  } else rb.classList.remove('show');

  // hero — single source of overall status.
  // hicHTML holds the icon markup; it is written to the DOM only when it actually
  // changes (see the guarded assignment below). Rebuilding the icon on every frame would
  // insert a fresh <svg> and restart its CSS animation from 0% — making the pulsing
  // sleep/charge/wake ring visibly jump on each ~2 s WS push instead of fading smoothly.
  var hic=$("hicon"), hl=$("hlabel"), hs=$("hsub"), hst=$("hstats"), hicHTML=null;
  $("hero").classList.remove('hide');   // shown for every paired state; unknown/unreachable now show a grey status hero (not hidden)
  hst.innerHTML='';   // cleared here; only the paired+reporting branch populates it
  if(paired&&v){
    if(waking){ waking=false; clearTimeout(wakeTimeout); }   // car is awake & reporting — stop the spinner
    var soc=v.soc!=null?v.soc:0, col=socColor(soc);
    // "Charge complete" = battery reached its target and not charging; the start tap is gated
    // (the car would only reject a charge_start here as "complete"). See chargeComplete().
    var complete=chargeComplete(v,charging);
    var ringHTML=charging?ringCharge(soc,col):ringSVG(soc,false,col);   // spin while charging, static when complete
    var ctitle=charging?'Tap to stop charging':(complete?'Battery full — already charged':'Tap to start charging');
    hicHTML='<button class="wakebtn" onclick="toggleCharge()" title="'+ctitle+'" aria-label="'+ctitle+'"'+((chgBusy||complete)?' disabled':'')+'>'+ringHTML+'<div class="pct"><b style="color:'+col+'">'+soc+'<small>%</small></b></div></button>';
    setHTML(hl,'<span>'+(complete?'Charge complete':esc(v.status||'Idle'))+'</span>');
    hs.textContent='';
    hst.innerHTML=chargeStats(v,charging)+copChip(s)+defrostChip(s);
  } else if(paired){
    if(waking){
      hicHTML='<button class="wakebtn" disabled aria-label="Waking the car">'+ringSleep(104,true)+'<span class="glyph">'+ALARM+'</span></button>';
      setHTML(hl,'<span>Waking up…</span>');
      hs.textContent='Reaching your Tesla over Bluetooth…';
    } else if(s.link==='asleep'){
      // Proven reachable-but-sleeping: the always-on VCSEC health poll answered recently, so
      // "asleep" is a fact and the wake button is meaningful. Show the card.
      hicHTML='<button class="wakebtn" onclick="wakeCar()" title="Tap to wake the car" aria-label="Wake the car">'+ringSleep()+'<span class="glyph">'+MOON+'</span></button>';
      setHTML(hl,'<span>Vehicle asleep</span>');
      hs.textContent='Tap the icon to wake the car.';
      // Last-known battery + how long the car has been asleep, from the retained cache —
      // shown without waking the car (last reading; a parked car barely drains).
      var ls=s.last||{}, lsoc=(ls.soc!=null)?Math.round(ls.soc):null, ago=fmtAgo(s.last_seen_s), chips=[];
      if(lsoc!=null) chips.push(stat('Battery','<span style="color:'+socColor(lsoc)+'">'+lsoc+'</span>','%'));
      if(ago)        chips.push(stat('Idle', ago, ''));
      // No Overheat/Defrost chips here: their value is the live AC draw, which exists only while
      // the car is awake and reporting. Asleep there is nothing to show, and we never wake the
      // car to obtain it.
      hst.innerHTML=chips.join('');
    } else if(s.link==='idle'){
      // Reachable over BLE but NOT provably asleep: we stopped polling the infotainment domain
      // to let the car sleep, and the car's VCSEC sleep flag hasn't held ASLEEP long enough to
      // confirm. We honestly don't know if it's awake or asleep, so we must NOT claim "asleep".
      // Show a neutral standby card (last-known battery + idle time) that still offers the wake
      // button — tapping it fetches a fresh reading (and flips to the live SOC card if awake).
      hicHTML='<button class="wakebtn" onclick="wakeCar()" title="Tap to wake the car" aria-label="Wake the car">'+ringSleep()+'<span class="glyph">'+PARKED+'</span></button>';
      setHTML(hl,'<span>Parked</span>');
      hs.textContent='No live reading — tap the icon to wake the car.';
      var ils=s.last||{}, ilsoc=(ils.soc!=null)?Math.round(ils.soc):null, iago=fmtAgo(s.last_seen_s), ichips=[];
      if(ilsoc!=null) ichips.push(stat('Battery','<span style="color:'+socColor(ilsoc)+'">'+ilsoc+'</span>','%'));
      if(iago)        ichips.push(stat('Idle', iago, ''));
      // No Overheat/Defrost chips here (same as the asleep card): both key off the live AC
      // draw (liveKw needs s.vehicle), and /status emits "vehicle" only while link==='awake'.
      hst.innerHTML=ichips.join('');
    } else if((s.ble&&s.ble.connect_fail)>=2){
      // Paired, but the BLE link won't come up despite repeated attempts. Surface WHY — exactly
      // like the setup flow — instead of hiding the hero (which looks like the car is off/away).
      // car_connectable===false ⇒ the car is at its ~3-device BLE limit (no slot for us).
      var pcc=s.ble.car_connectable;
      hicHTML=ringSVG(0,true)+'<div class="glyph">'+BT+'</div>';
      setHTML(hl,'<span>Connection failed</span>');
      hs.textContent=(pcc===false)
        ? 'The car has too many Bluetooth devices connected.'
        : 'Move the device closer, or disconnect other devices using the car.';
    } else {
      // paired, BLE may be up, but link_state is 'unknown' (nothing heard since boot — the
      // on-demand link hasn't completed a signed round-trip yet) or 'unreachable' (heard
      // before, now stale: drove off / out of range / deep sleep). We never claim "asleep"
      // when we don't know — but instead of a blank area we show a neutral grey hero with the
      // orange BLE glyph (matching the orange BLE bars) and the honest state, plus the
      // last-known battery + idle time from the retained cache so the card still informs.
      var ureach=s.link==='unreachable';
      hicHTML=ringSVG(0,true)+'<div class="glyph">'+BT+'</div>';
      // link==='awake' can land here transiently when the freshness stamp arrived from a
      // non-charge poll before the first charge poll filled the cache (no s.vehicle yet) —
      // say "Checking status…" then, not "Connecting…" (the link is clearly up).
      setHTML(hl,'<span>'+(ureach?'Unreachable':(s.link==='awake'?'Checking status…':'Connecting…'))+'</span>');
      // Only claim "Bluetooth connected" when the momentary GATT link is actually up
      // (linked). The on-demand link is dropped between polls, so link==='unknown'
      // routinely coexists with ble.connected===false — in which case the BLE row reads
      // "Disconnected" and a "Bluetooth connected" subtitle would contradict it.
      hs.textContent=ureach
        ? 'No recent response from the vehicle over Bluetooth.'
        : (linked ? 'Bluetooth connected — checking status…'
                  : 'Reaching your Tesla over Bluetooth…');
      var uls=s.last||{}, ulsoc=(uls.soc!=null)?Math.round(uls.soc):null, uago=fmtAgo(s.last_seen_s), uchips=[];
      if(ulsoc!=null) uchips.push(stat('Battery','<span style="color:'+socColor(ulsoc)+'">'+ulsoc+'</span>','%'));
      if(uago)        uchips.push(stat('Idle', uago, ''));
      hst.innerHTML=uchips.join('');
    }
  } else if(!configured){
    hicHTML=ringSVG(0,true)+'<div class="glyph">'+BTG+'</div>';
    setHTML(hl,'<span>Set up needed</span>');
    hs.textContent=hasVin?'Generate a security key below.':'Add the vehicle VIN below to begin.';
  } else if(linked){
    hicHTML=ringSVG(0,true)+'<div class="glyph">'+CARD+'</div>';
    setHTML(hl,'<span>Pairing</span>');
    hs.textContent='Approve the request on your Tesla’s touchscreen.';
  } else if((s.ble&&s.ble.connect_fail)>=2){
    // The car WAS found (advert heard, name matches the VIN) but every connection attempt
    // times out. Don't blame range — the signal is fine. Distinguish the two real causes the
    // same way Tesla's own vehicle-command does, via the advert's connectable flag:
    //   car_connectable===false → the car advertises non-connectable ⇒ it's at its BLE
    //     connection limit (~3 keys/devices), so no slot is free.
    //   otherwise → it accepts connections but the link still fails ⇒ weak signal or another
    //     proxy contending for the link.
    var cc=s.ble.car_connectable;
    hicHTML=ringSVG(0,true)+'<div class="glyph">'+BT+'</div>';
    setHTML(hl,'<span>Connection failed</span>');
    hs.textContent=(cc===false)
      ? 'The car has too many Bluetooth devices connected.'
      : 'Move the device closer, or disconnect other devices using the car.';
  } else {
    hicHTML=ringSVG(0,true)+'<div class="glyph">'+BOLT+'</div>';
    setHTML(hl,'<span>Looking for your car</span>');
    hs.textContent='Bring the device within Bluetooth range.';
  }
  // Only touch the DOM when the icon markup changed — otherwise the running ring
  // animation keeps its phase and fades smoothly instead of restarting each poll.
  if(hicHTML!==null) setHTML(hic,hicHTML);
  hs.style.display=hs.textContent?'':'none';
  equalizeChips(hst);   // all hero chips share the widest one's width (set after hst is populated)

  // tiles ---------------------------------------------------------
  // vehicle
  $("vehVal").innerHTML=hasVin?esc(s.vin):'<span class="ph">Not set</span>';
  var pairedTxt=s.paired_at?('Paired '+new Date(s.paired_at*1000).toLocaleDateString(undefined,{year:'numeric',month:'short',day:'numeric'})):'paired';
  setSub("vehSub", paired?pairedTxt:(hasVin?'not paired':'tap to add VIN'), '');

  // bluetooth — link to the car
  var ble=s.ble||{}, bc=$("bleConn");
  if(!hasVin){
    // No VIN configured: the device never connects/enrols (pairing is gated off), it only
    // discovers. List every nearby Tesla, sorted by signal strength, in the SAME layout as the
    // connected row: bars, dBm, then MAC. While the first scan is still running with nothing
    // seen yet, fall back to the "Searching…" animation.
    var nd=ble.devices||[];
    if(nd.length){
      bc.className='cv';
      bc.innerHTML='<div class="btlist">'+nd.map(function(d){
        return '<div class="btrow">'+barsHTML(d.rssi)+
               '<span class="dbm">'+d.rssi+' dBm</span>'+
               '<span class="nm mac">'+esc(d.addr)+'</span></div>';
      }).join('')+'</div>';
    } else {
      bc.className='cv'; setHTML(bc,searchBarsHTML()+'<span>Searching…</span>');
    }
  } else if(linked){
    // The GATT link is up. If we're paired but have no fresh telemetry — link_state
    // unknown/unreachable, the case where the hero shows a grey status card — the link is
    // connected yet stateless: drive the bars with the orange wave (waveBarsHTML) to
    // signal "status unknown" instead of a healthy green. Awake/idle/asleep (and the
    // pre-pair "Pairing" state) are known states → plain green bars as before.
    var unknownStatus=paired&&(s.link==='unknown'||s.link==='unreachable');
    var bbars=unknownStatus?waveBarsHTML()
             :(ble.rssi!=null)?barsHTML(ble.rssi):'';                          // bars (signal glyph)
    var bdbm=(ble.rssi!=null)?'<span class="dbm">'+ble.rssi+' dBm</span>':'';  // numeric reading
    var btxt=ble.addr?'<span class="nm mac">'+esc(ble.addr)+'</span>':'';
    bc.className=unknownStatus?'cv unkn':'cv ok';   // orange MAC under the ping-pong; green when known
    setHTML(bc,bbars+bdbm+btxt);
  } else if(ble.connect_fail>=2){
    // Car found but the link keeps timing out (see the hero) — show it as a warning on the
    // Bluetooth row rather than the neutral "Searching…", which would imply it can't find the
    // car. Show real signal bars + dBm (the signal itself is fine — the link slot is the
    // problem), and name the reason (non-connectable ⇒ at the car's BLE-connection limit).
    // ble.rssi is the last-seen advert RSSI; fall back to the strongest entry in devices[].
    var sr=(ble.rssi!=null)?ble.rssi:(ble.devices||[]).reduce(function(a,d){return d.rssi>a?d.rssi:a;},null);
    var sdbm=(sr!=null)?'<span class="dbm">'+sr+' dBm</span>':'';
    var rt=(ble.car_connectable===false)?'At connection limit':'Connection failed';
    bc.className='cv warn'; setHTML(bc,barsHTML(sr)+sdbm+'<span>'+rt+'</span>');
  } else if(ble.scanning){
    // searching for the car — the signal bars sit where they do when connected, but fill up
    // green one after another (CSS .bars.search animation) instead of a spinner; it appears
    // only while scanning and clears otherwise. setHTML keeps the bar nodes alive across
    // WS pushes so the fill animation runs continuously instead of restarting each ~2 s.
    bc.className='cv'; setHTML(bc,searchBarsHTML()+'<span>Searching…</span>');
  } else {
    // disconnected — still show the (faded, empty) bar glyph so the row keeps the same
    // shape as Wi-Fi rather than collapsing to text only.
    bc.className='cv'; setHTML(bc,barsHTML(null)+'<span class="ph">Disconnected</span>');
  }

  // MQTT — Home Assistant bridge (tap to set/change the broker)
  var mq=s.mqtt||{}, mc=$("mqttConn");
  if(!mq.configured){ mc.className='cv'; mc.innerHTML='<span class="ph">Not configured</span>'; }
  else if(mq.connected){ mc.className='cv ok'; mc.innerHTML='<span>'+esc(mq.broker||'')+(mq.tls?' · secured':'')+'</span>'; }
  else { mc.className='cv warn'; mc.innerHTML='<span>'+esc(mq.broker||'')+' · '+esc(mq.error||'disconnected')+'</span>'; }

  // Syslog — UDP diag-log forwarder (tap to set/change the server). Delivery is
  // gated on DNS only (resolved); reachability is an advisory ping hint, so a
  // resolved-but-not-answering host still shows the destination, warn-flagged.
  var sy=s.syslog||{}, sc=$("syslogConn");
  if(!sy.configured){ sc.className='cv'; sc.innerHTML='<span class="ph">Not configured</span>'; }
  else if(sy.error){ sc.className='cv warn'; sc.innerHTML='<span>'+esc(sy.host?(sy.host+':'+(sy.port||514)):'')+' · '+esc(sy.error)+'</span>'; }
  else if(sy.resolved){ sc.className='cv'+(sy.reachable?' ok':' warn'); sc.innerHTML='<span>'+esc(sy.host+':'+(sy.port||514))+(sy.reachable?'':' · not answering ping')+'</span>'; }
  else { sc.className='cv warn'; sc.innerHTML='<span>Resolving…</span>'; }

  // header meta line: IP · version (IP first)
  $("ipline").innerHTML = s.ip ? esc(s.ip)+'&nbsp;·&nbsp;' : '';   // nbsp: overflow:hidden trims plain trailing spaces
  var vl=$("verLink");
  vl.textContent='v'+(s.version||'?');
  vl.className='verlink';   // always grey — never the accent-red highlight
  vl.title=otaAvail?('Update '+otaAvail+' available — tap to install'):'Tap to check for updates';

  // key
  if(s.key_present){
    $("keyVal").innerHTML='<code>'+esc(s.key_fingerprint||'')+'</code>';
    setSub("keySub", s.key_created?('Created '+new Date(s.key_created*1000).toLocaleDateString(undefined,{year:'numeric',month:'short',day:'numeric'})):'','');
  } else {
    $("keyVal").innerHTML='<span class="ph">Not generated</span>';
    setSub("keySub",'tap to generate','');
  }
}
function setSub(id,txt,cls){ var e=$(id); e.className='tsub'+(cls?' '+cls:''); e.textContent=txt; }

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
  toast(isCharging?'Stopping charge…':'Starting charge…','info');
  fetch('/api/1/vehicles/'+encodeURIComponent(vin)+'/command/'+cmd,{method:'POST'})
    .then(function(r){return r.json()})
    .then(function(j){
      var ok=!!(j&&j.response&&j.response.result);
      if(ok){ toast(isCharging?'Charging stopped':'Charging started','ok'); return; }
      var f=chargeFailMsg((j&&j.response&&j.response.reason)||'', isCharging);
      toast(f.msg, f.type);
    })
    .catch(function(){ toast('Command failed — is the car in range?','err'); })
    .then(function(){ chgBusy=false; poll(); });
}
// turn the device's failure reason into a clear message; the car rejecting a command
// (e.g. "complete") is not a connectivity problem, so don't blame the BLE range.
function chargeFailMsg(reason,isCharging){
  var r=String(reason).toLowerCase();
  if(r.indexOf('not reachable')>=0||r.indexOf('timed out')>=0||r.indexOf('unreachable')>=0)
    return {msg:'Car not reachable — is it in range?', type:'err'};
  if(r.indexOf('complete')>=0)        return {msg:'Charging is already complete', type:'info'};
  if(r.indexOf('not_charging')>=0||r.indexOf('not charging')>=0)
                                      return {msg:'The car isn’t charging', type:'info'};
  if(r.indexOf('is_charging')>=0)     return {msg:'The car is already charging', type:'info'};
  if(r.indexOf('disconnected')>=0||r.indexOf('cable')>=0||r.indexOf('unplug')>=0)
                                      return {msg:'No charge cable connected', type:'info'};
  var bare=String(reason).replace(/^.*action failed:\s*/i,'').trim();
  return {msg:(isCharging?'Couldn’t stop charging':'Couldn’t start charging')+(bare?' — '+bare:''), type:'err'};
}

/* ---------- config actions ---------- */
function vinValid(v){return /^[A-HJ-NPR-Z0-9]{17}$/i.test(v)}
function editVin(){
  var cur=(state&&state.vin&&state.vin!=='UNKNOWN')?state.vin:'';
  var v=prompt('Tesla VIN (17 characters):',cur); if(v==null)return;
  v=v.trim().toUpperCase();
  if(!vinValid(v)){ toast('Invalid VIN — must be 17 characters','err'); return; }
  if(v===(state&&state.vin)){ toast('VIN unchanged','info'); return; }
  toast('Saving VIN…','info');
  fetch('/set_vin',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({vin:v})})
    .then(function(r){return r.json()})
    .then(function(j){var o=(j&&j.response)||{};
      if(!o.result){ toast(o.reason||'Failed to save VIN','err'); return; }
      if(/no reboot|unchanged/i.test(o.reason||'')){ toast('VIN unchanged','info'); return; }
      toast('VIN saved · rebooting','ok');})
    .catch(function(){toast('Saved — device rebooting','info')});
}
function editMqtt(){
  var cur=(state&&state.mqtt&&state.mqtt.broker)?state.mqtt.broker:'';
  var v=prompt('MQTT broker for Home Assistant (IP:PORT, e.g. 192.168.1.10:1883).\nLeave empty to disable.',cur);
  if(v==null) return;
  v=v.trim();
  if(v && v.indexOf(' ')>=0){ toast('Invalid broker — use IP:PORT','err'); return; }
  if(v===cur){ toast(v?'MQTT broker unchanged':'MQTT already disabled','info'); return; }
  toast(v?'Saving MQTT broker…':'Disabling MQTT…','info');
  fetch('/set_mqtt',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({broker:v})})
    .then(function(r){return r.json()})
    .then(function(j){var o=(j&&j.response)||{};
      if(!o.result){ toast(o.reason||'Failed to save MQTT broker','err'); return; }
      if(/no reboot|unchanged|already/i.test(o.reason||'')){ toast(v?'MQTT broker unchanged':'MQTT already disabled','info'); return; }
      toast('Saved · rebooting','ok');})
    .catch(function(){toast('Saved — device rebooting','info')});
}
function editSyslog(){
  var sy=state&&state.syslog, cur=(sy&&sy.host)?(sy.host+':'+(sy.port||514)):'';
  var v=prompt('Syslog server for the diagnostic log (IP:PORT, e.g. 192.168.1.22:514).\nLeave empty to disable.',cur);
  if(v==null) return;
  v=v.trim();
  if(v && v.indexOf(' ')>=0){ toast('Invalid server — use IP:PORT','err'); return; }
  if(v===cur){ toast(v?'Syslog server unchanged':'Syslog already disabled','info'); return; }
  toast(v?'Saving Syslog server…':'Disabling Syslog…','info');
  fetch('/set_syslog',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({server:v})})
    .then(function(r){return r.json()})
    .then(function(j){var o=(j&&j.response)||{};
      if(!o.result){ toast(o.reason||'Failed to save Syslog server','err'); return; }
      if(/no reboot|unchanged|already/i.test(o.reason||'')){ toast(v?'Syslog server unchanged':'Syslog already disabled','info'); return; }
      toast('Saved · rebooting','ok');})
    .catch(function(){toast('Saved — device rebooting','info')});
}
function wakeStop(){ waking=false; clearTimeout(wakeTimeout); }
function wakeCar(){
  if(waking) return;
  var vin=state&&state.vin;
  if(!vin||vin==='UNKNOWN'){ toast('No VIN configured','err'); return; }
  waking=true; if(state)render(state);                       // ring starts spinning immediately
  clearTimeout(wakeTimeout);
  // safety net: stop spinning if no charge data shows up in time
  wakeTimeout=setTimeout(function(){ if(waking){ wakeStop(); toast('Still asleep — try again','info'); poll(); } }, 90000);
  toast('Waking the car…','info');
  fetch('/api/1/vehicles/'+encodeURIComponent(vin)+'/command/wake_up',{method:'POST'})
    .then(function(r){return r.json()})
    .then(function(j){
      var ok=!!(j&&j.response&&j.response.result);
      if(ok){ toast('Wake sent · waiting for the car…','ok'); poll(); }   // keep spinning until SOC arrives
      else { wakeStop(); toast('Wake failed — is the car in range?','err'); poll(); }
    })
    .catch(function(){ wakeStop(); toast('Wake failed — is the car in range?','err'); poll(); });
}
function genKey(){
  if(state&&state.key_present && !confirm('Regenerate the security key?\n\nThe current key is invalidated and you must re-pair with the vehicle.')) return;
  toast('Generating new key…','info');
  fetch('/gen_keys?force=1',{method:'POST'})
    .then(function(){toast('New key generated · re-pair with the vehicle','ok'); poll();})
    .catch(function(){toast('Key generation failed','err')});
}

/* ---------- OTA ---------- */
// Status shows inline in the header meta line (progress ring + text), next to the
// version — no bottom popup. A tiny ring sized for the 13.5px meta line.
var otaBusy=false;
// col = arc colour (default the brand accent, used by OTA). The BLE "searching" ring passes
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
function otaInline(html,cls){
  var el=$("otaStat"); if(!el)return;
  el.innerHTML=html||''; el.className='otastat'+(cls?' '+cls:'');
}
function otaInlineClear(delay){ setTimeout(function(){ otaInline(''); }, delay||3000); }
function otaCheck(){
  if(otaBusy) return;              // a check/update is already running
  otaBusy=true;
  otaInline(otaMiniRing(0,true,'currentColor'));   // checking — spinning ring only, no label
  fetch('/ota/check?ms='+Date.now()).then(otaCheckPoll).catch(function(){ otaBusy=false; otaInline('<span>check failed</span>','err'); otaInlineClear(4000); });
}
function otaCheckPoll(){
  clearTimeout(otaTimer);
  fetch('/ota/status').then(function(r){return r.json()}).then(function(o){
    if(o.state==='checking'){ otaTimer=setTimeout(otaCheckPoll,1200); return; }
    if(o.state==='downloading'||o.state==='done'){ otaProgress(o); return; }   // already updating
    if(o.state==='error'){ otaBusy=false; otaInline('<span>check failed</span>','err'); otaInlineClear(4000); return; }
    if(o.update_available){
      otaAvail=o.available||''; if(state)render(state); otaInline('');         // clear while the dialog is up
      if(confirm('New version '+(o.available||'')+' available — update now?\n\nYou’re on v'+(o.current||'')+'. The device downloads the firmware and reboots when done.')){
        otaInline(otaMiniRing(0,true,'currentColor')+'<span>starting…</span>');
        fetch('/ota/update',{method:'POST'}).then(otaPoll).catch(function(){otaPoll()});
      } else { otaAvail=null; if(state)render(state); otaBusy=false; }   // cancelled → version back to grey
    } else {
      otaAvail=null; if(state)render(state); otaBusy=false;
      otaInline('<span>up to date</span>'); otaInlineClear(3500);
    }
  }).catch(function(){ otaBusy=false; otaInline('<span>check failed</span>','err'); otaInlineClear(4000); });
}
function otaPoll(){
  clearTimeout(otaTimer);
  fetch('/ota/status').then(function(r){return r.json()}).then(otaProgress).catch(function(){ otaTimer=setTimeout(otaPoll,1500); });
}
function otaProgress(o){
  otaBusy=true;
  if(o.state==='downloading'){ var p=o.progress||0; otaInline(otaMiniRing(p,false,'currentColor')+'<span>'+p+'%</span>'); otaTimer=setTimeout(otaPoll,800); }
  else if(o.state==='done'){ otaInline(otaMiniRing(100,false,'currentColor')+'<span>rebooting…</span>'); setTimeout(function(){waitReboot(state&&state.version)},2000); }
  else if(o.state==='error'){ otaBusy=false; otaInline('<span>update failed'+(o.message?' — '+esc(o.message):'')+'</span>','err'); otaInlineClear(6000); }
  else { otaTimer=setTimeout(otaPoll,1000); }
}
function resumeOta(){
  fetch('/ota/status').then(function(r){return r.json()}).then(function(o){
    if(o&&(o.state==='downloading'||o.state==='done')){ otaBusy=true; otaProgress(o); }
  }).catch(function(){});
}
// After "done" the device reboots ~1.2s later. Poll /status in the background and
// reload once it's back on the NEW image. We can't rely only on seeing it go *down*
// — by the time we start polling it may already be back online — so a firmware
// version change is the primary "rebooted" signal, with a down→up transition and an
// overall timeout as fallbacks. The per-request abort keeps polling responsive while
// the device is unreachable instead of hanging on a dead socket.
function waitReboot(preVer){
  var sawDown=false, started=Date.now();
  (function probe(){
    var ctl=('AbortController' in window)?new AbortController():null;
    var to=ctl?setTimeout(function(){ctl.abort()},3000):null;
    fetch('/status?ms='+Date.now(),{cache:'no-store',signal:ctl?ctl.signal:undefined})
      .then(function(r){ if(!r.ok)throw 0; return r.json(); })
      .then(function(o){ if(to)clearTimeout(to);
        if((preVer&&o&&o.version&&o.version!==preVer)||sawDown){ location.reload(); return; }
        next();
      })
      .catch(function(){ if(to)clearTimeout(to); sawDown=true; next(); });
    function next(){
      if(Date.now()-started>90000){ location.reload(); return; }   // safety net
      setTimeout(probe,1000);
    }
  })();
}

/* ---------- boot ---------- */
function boot(){
  fetch('/set_time',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ms:Date.now()})}).catch(function(){});
  if(window.WebSocket){
    connectWS();                 // live feed: one socket, device pushes /status — no interval poll
  } else {
    // No WebSocket in this (very old) browser: fetch one snapshot so the page isn't blank, then
    // stop. Deliberately no polling — the live UI is WebSocket-only; reload the page to refresh.
    fetch('/status?ms='+Date.now(),{cache:'no-store'}).then(function(r){return r.json()}).then(render).catch(function(){});
  }
  resumeOta();
}
boot();
