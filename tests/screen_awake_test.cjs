const assert=require('node:assert/strict');
const {test}=require('node:test');
const {readFileSync}=require('node:fs');
const {join}=require('node:path');
const vm=require('node:vm');
const script=readFileSync(join(__dirname,'../web/screen-awake.js'),'utf8');
const flush=()=>new Promise(resolve=>setImmediate(resolve));

function setup({native=true,stored=null,storageFails=false,request=null,playFails=false}={}){
  const events={},windowEvents={},toggleEvents={},values=new Map(stored===null?[]:[['airbike.keepScreenAwake',stored]]);
  const toggle={checked:false,addEventListener:(name,fn)=>toggleEvents[name]=fn},status={textContent:''};
  const locks=[];let requests=0,plays=0,pauses=0;
  function makeLock(){
    const listeners={};const lock={released:false,addEventListener:(name,fn)=>listeners[name]=fn,
      async release(){this.released=true;listeners.release?.();}};
    locks.push(lock);return lock;
  }
  const videoEvents={};
  const video={style:{},duration:60,currentTime:0,sources:[],setAttribute(){},appendChild(s){this.sources.push(s);},
    addEventListener:(name,fn)=>videoEvents[name]=fn,
    play(){++plays;return playFails?Promise.reject(new Error('Blocked')):Promise.resolve();},
    pause(){++pauses;videoEvents.pause?.();}};
  const document={visibilityState:'visible',getElementById:id=>id==='keep-awake'?toggle:status,
    addEventListener:(name,fn)=>events[name]=fn,createElement:type=>type==='video'?video:{},body:{appendChild(){}}};
  const navigator=native?{wakeLock:{request(type){assert.equal(type,'screen');++requests;return request?request(makeLock):Promise.resolve(makeLock());}}}:{};
  const localStorage={getItem(key){if(storageFails)throw Error();return values.get(key)??null;},setItem(key,value){if(storageFails)throw Error();values.set(key,value);}};
  vm.runInNewContext(script,{document,navigator,localStorage,noSleepMedia:{webm:'data:video/webm;base64,test',mp4:'data:video/mp4;base64,test'},window:{addEventListener:(name,fn)=>windowEvents[name]=fn}});
  return {document,events,windowEvents,toggle,status,locks,values,video,videoEvents,
    counts:()=>({requests,plays,pauses}),async change(on){toggle.checked=on;toggleEvents.change();await flush();}};
}

test('enabled by default and acquires native lock; disabling releases and persists',async()=>{
  const h=setup();await flush();assert.equal(h.toggle.checked,true);assert.match(h.status.textContent,/Active/);
  assert.equal(h.counts().requests,1);await h.change(false);
  assert.equal(h.locks[0].released,true);assert.equal(h.values.get('airbike.keepScreenAwake'),'0');assert.match(h.status.textContent,/Off/);
});
test('saved opt-out does not acquire on load or gestures; enabling works',async()=>{
  const h=setup({stored:'0'});h.events.pointerdown();await flush();assert.equal(h.counts().requests,0);
  await h.change(true);assert.equal(h.counts().requests,1);assert.equal(h.values.get('airbike.keepScreenAwake'),'1');
});
test('hide releases; visible reacquires; OS release waits for retry gesture',async()=>{
  const h=setup();await flush();h.document.visibilityState='hidden';h.events.visibilitychange();await flush();
  assert.equal(h.locks[0].released,true);assert.match(h.status.textContent,/Paused/);
  h.document.visibilityState='visible';h.events.visibilitychange();await flush();assert.equal(h.locks.length,2);
  await h.locks[1].release();assert.match(h.status.textContent,/Tap/);
  h.events.pointerdown();await flush();assert.equal(h.locks.length,3);
  h.windowEvents.pagehide();await flush();assert.equal(h.locks[2].released,true);
});
test('disable during pending request releases late lock and never reports active',async()=>{
  let finish;const h=setup({request:make=>new Promise(resolve=>finish=()=>resolve(make()))});
  await h.change(false);finish();await flush();assert.equal(h.locks[0].released,true);assert.match(h.status.textContent,/Off/);
});
test('failed native request is reported and can retry on a gesture',async()=>{
  let fail=true;const h=setup({request:make=>fail?Promise.reject(new Error('Denied')):Promise.resolve(make())});
  await flush();assert.match(h.status.textContent,/Not active/);fail=false;h.events.pointerdown();await flush();assert.match(h.status.textContent,/Active/);
});
test('leaving during an in-flight acquisition cannot reacquire until pageshow',async()=>{
  let finish;const h=setup({request:make=>new Promise(resolve=>finish=()=>resolve(make()))});
  h.windowEvents.pagehide();finish();await flush();assert.equal(h.counts().requests,1);
  assert.equal(h.locks[0].released,true);assert.match(h.status.textContent,/Paused/);
  h.windowEvents.pageshow();assert.equal(h.counts().requests,2);finish();await flush();assert.match(h.status.textContent,/Active/);
});
test('HTTP fallback waits for a tap, plays local silent media and pauses when hidden',async()=>{
  const h=setup({native:false});await flush();assert.equal(h.counts().plays,0);assert.match(h.status.textContent,/Tap/);
  h.events.pointerdown();await flush();assert.equal(h.counts().plays,1);assert.equal(h.video.muted,true);
  assert.ok(h.video.sources.every(s=>s.src.startsWith('data:video/')));assert.match(h.status.textContent,/fallback running/);
  h.video.currentTime=1;h.videoEvents.timeupdate();assert.equal(h.video.currentTime,0);
  h.document.visibilityState='hidden';h.events.visibilitychange();assert.ok(h.counts().pauses>0);
  h.document.visibilityState='visible';h.events.visibilitychange();await flush();assert.match(h.status.textContent,/Tap/);
});
test('fallback rejection and unavailable storage are honest and nonfatal',async()=>{
  const h=setup({native:false,storageFails:true,playFails:true});h.events.pointerdown();await flush();
  assert.match(h.status.textContent,/Not active/);assert.match(h.status.textContent,/cannot be saved/);
  await h.change(false);assert.match(h.status.textContent,/Off/);
});
