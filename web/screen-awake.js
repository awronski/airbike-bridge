// A screen preference belongs to this browser, independently of rider/ride settings.
function initScreenAwake() {
  const toggle=document.getElementById('keep-awake');
  const status=document.getElementById('awake-status');
  const key='airbike.keepScreenAwake';
  let enabled=true,saved=true,lock=null,video=null,playing=false,busy=false,generation=0,pageActive=true;
  try { enabled=localStorage.getItem(key)!=='0'; } catch (_) { saved=false; }
  toggle.checked=enabled;
  const native=!!navigator.wakeLock?.request;
  function message(value) { status.textContent=value+(saved?'':' Preference cannot be saved in this browser.'); }
  function idleMessage() {
    message(!enabled?'Off · normal screen timeout.':!pageActive||document.visibilityState!=='visible'?
      'Paused while the page is hidden.':'Tap the dashboard to keep the screen awake.');
  }
  function stop() {
    ++generation;
    const old=lock;lock=null;
    if(old)old.release().catch(()=>{});
    playing=false;if(video)video.pause();
    idleMessage();
  }
  function playbackVideo() {
    if(video)return video;
    // Local, silent media from NoSleep.js v0.12.0 (MIT). No network playback.
    video=document.createElement('video');video.muted=true;video.loop=true;
    video.setAttribute('playsinline','');video.setAttribute('aria-hidden','true');
    video.style.cssText='position:fixed;width:1px;height:1px;left:-10px;top:-10px;pointer-events:none';
    for(const type of ['webm','mp4']){
      const source=document.createElement('source');source.src=noSleepMedia[type];source.type='video/'+type;video.appendChild(source);
    }
    video.addEventListener('timeupdate',()=>{if(video.duration>1&&video.currentTime>.5)video.currentTime=0;});
    video.addEventListener('pause',()=>{playing=false;if(enabled&&!lock)idleMessage();});
    video.addEventListener('error',()=>{playing=false;message('Screen awake unavailable · use the device’s screen timeout setting.');});
    document.body.appendChild(video);return video;
  }
  async function request(fromGesture=false) {
    if(!enabled||!pageActive||document.visibilityState!=='visible'||busy||lock||playing)return;
    if(!native&&!fromGesture){idleMessage();return;}
    const version=generation;busy=true;
    try {
      if(native){
        const acquired=await navigator.wakeLock.request('screen');
        if(version!==generation||!enabled||!pageActive||document.visibilityState!=='visible'){await acquired.release();return;}
        lock=acquired;
        acquired.addEventListener('release',()=>{
          if(lock!==acquired)return;lock=null;idleMessage();
        });
        message('Active · screen wake lock.');
      }else{
        const helper=playbackVideo();await helper.play();
        if(version!==generation||!enabled||!pageActive||document.visibilityState!=='visible'){helper.pause();return;}
        playing=true;message('Playback fallback running · keep this page open.');
      }
    }catch(_){
      if(version!==generation||!enabled||!pageActive||document.visibilityState!=='visible')idleMessage();
      else message('Not active · tap to retry. The browser or power-saving mode may block it.');
    }finally{
      busy=false;
      if(version!==generation&&native&&enabled&&pageActive&&document.visibilityState==='visible')request();
    }
  }
  toggle.addEventListener('change',()=>{
    enabled=toggle.checked;
    try {localStorage.setItem(key,enabled?'1':'0');saved=true;}catch(_){saved=false;}
    if(enabled)request(true);else stop();
  });
  document.addEventListener('visibilitychange',()=>{
    if(document.visibilityState==='visible')request();else stop();
  });
  window.addEventListener('pagehide',()=>{pageActive=false;stop();});
  window.addEventListener('pageshow',()=>{pageActive=true;request();});
  document.addEventListener('pointerdown',()=>request(true),{passive:true});
  document.addEventListener('keydown',event=>{if(event.key!=='Escape')request(true);});
  idleMessage();request();
}
initScreenAwake();
