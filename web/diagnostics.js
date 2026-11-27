/* Local diagnostic controls. Collection happens on the ATOM, not in this page. */
var AirBikeDiagnostics = (() => {
  const seconds = ms => (Number(ms || 0) / 1000).toFixed(1) + ' s';
  function describe(e) {
    const who = {bike:'Bike',polar:'Polar',garmin:'Garmin',wifi:'Wi-Fi',atom:'ATOM',storage:'Storage'}[e.source] || e.source;
    const code = '0x' + (Number(e.a) >>> 0).toString(16);
    if(e.kind === 'disconnect') {
      const reason = e.source === 'wifi' ? 'Wi-Fi reason' : ({520:'Connection timeout',531:'Remote termination',534:'Local termination'}[e.a] || 'Reason');
      const intent = e.b ? ' · intentional: ' + ({1:'band change',2:'subscription failed',3:'band selection changed'}[e.b] || 'local action') : '';
      return `${who}: ${reason} ${code}${intent}`;
    }
    if(e.kind === 'gap_start') return `${who}: waiting for measurements${e.b ? ' · Bluetooth connected' : ''}`;
    if(e.kind === 'gap_recovered') return `${who}: measurements returned after ${seconds(e.a)}`;
    if(e.kind === 'stale') return `${who}: measurements stale (${seconds(e.a)})`;
    if(e.kind === 'connect_failed') return `${who}: connection failed (${code})`;
    if(e.kind === 'connect_done') return `${who}: connection operation took ${seconds(e.a)}`;
    if(e.kind === 'flash_write') return `Storage: ${e.c ? 'saved' : 'save failed'} · ${(e.b/1000).toFixed(1)} ms`;
    if(e.kind === 'task_delay') return `${e.a ? 'Relay' : 'Dashboard'} task gap · ${(e.b/1000).toFixed(1)} ms`;
    if(e.kind === 'subscribe') return `${who}: subscription ${e.b ? 'enabled' : 'unavailable'} (0x${Number(e.a).toString(16)})`;
    return `${who}: ${String(e.kind).replaceAll('_',' ')}`;
  }
  function savedText(d) {
    let result = !d.saved_available ? 'No saved report yet' : !d.saved_this_boot ? 'Report retained from a previous power-on' : `Saved ${Math.floor(d.save_age_ms/60000)} min ago`;
    if(['write_failed','verification_failed','insufficient_storage','invalid_saved_report'].includes(d.save_status)) result = 'Save failed: ' + d.save_status.replaceAll('_',' ') + ' · ' + result;
    return result;
  }
  let busy=false,saving=false,generation=0,lastEnabled=true;
  const byID=id=>document.getElementById(id);
  function render(d) {
    lastEnabled=!!d.enabled;byID('record-diagnostics').checked=lastEnabled;
    byID('record-diagnostics').disabled=saving;
    byID('diagnostic-recording').textContent=!d.enabled?'Recording off':d.active?'Recording this ride':d.has_session?'Capture finished':'Ready · waiting for pedalling';
    byID('diagnostic-setting-status').textContent=d.setting_storage_ok?'Saved on the ATOM. Applies to every browser.':'Preference could not be verified in storage.';
    byID('diagnostic-save').textContent=savedText(d);
    const s=d.stats||{};
    byID('diagnostic-counts').textContent=`Bike drops: ${s.bike_disconnects||0} · Measurement gaps: ${s.bike_gaps||0} · Longest: ${seconds(s.bike_max_gap_ms)}`;
    byID('diagnostic-coverage').textContent=`${d.partial?'Partial capture. ':''}${d.events_omitted?d.events_omitted+' older events omitted. ':''}Detailed timeline stays in RAM; the saved report keeps totals and 40 recent events.`;
    byID('diagnostic-saved-link').hidden=!d.saved_available;
    const rows=(d.events||[]).slice(-12).reverse().map(e=>{
      const row=document.createElement('li');
      const t=Math.floor(e.at_ms/1000);row.textContent=`${Math.floor(t/60)}:${String(t%60).padStart(2,'0')} · ${describe(e)}`;return row;
    });
    byID('diagnostic-events').replaceChildren(...rows);
  }
  async function request(url,options={}) {
    const response=await fetch(url,{cache:'no-store',signal:AbortSignal.timeout(options.method==='POST'?15000:4000),...options});
    if(!response.ok)throw new Error(await response.text());return response.json();
  }
  async function refresh() {
    if(busy||saving)return;busy=true;const token=generation;
    try {const data=await request('/api/diagnostics');if(token===generation)render(data);}
    catch(e){if(token===generation)byID('diagnostic-setting-status').textContent='Diagnostics unavailable. '+e.message;}
    finally{busy=false;}
  }
  function init() {
    const toggle=byID('record-diagnostics'),panel=byID('diagnostic-panel');if(!toggle||!panel)return;
    toggle.addEventListener('change',async()=>{
      const wanted=toggle.checked;saving=true;toggle.disabled=true;++generation;
      try{const data=await request('/api/diagnostics/settings',{method:'POST',headers:{'X-AirBike-Action':'diagnostics','Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({enabled:wanted?'1':'0'})});render(data);}
      catch(e){
        toggle.checked=lastEnabled;
        try{
          render(await request('/api/diagnostics'));
          byID('diagnostic-setting-status').textContent='Could not confirm the save. Showing the current preference.';
        }catch(_){
          byID('diagnostic-setting-status').textContent='Could not confirm the save. Refresh Settings to check the preference.';
        }
      }
      finally{saving=false;toggle.disabled=false;}
    });
    panel.addEventListener('toggle',()=>{if(panel.open)refresh();});
    setInterval(()=>{if(panel.open&&document.visibilityState==='visible')refresh();},5000);
  }
  if(typeof document!=='undefined')init();
  return {describe,savedText,refresh};
})();
