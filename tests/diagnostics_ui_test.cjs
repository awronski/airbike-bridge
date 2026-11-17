const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const source=fs.readFileSync('web/diagnostics.js','utf8');
function load(){const context={};vm.createContext(context);vm.runInContext(source,context);return context.AirBikeDiagnostics;}
test('reason descriptions preserve NimBLE category and intentional actions',()=>{
  const d=load();assert.match(d.describe({kind:'disconnect',source:'bike',a:0x208,b:0}),/timeout.*0x208/i);
  assert.doesNotMatch(d.describe({kind:'disconnect',source:'bike',a:8,b:0}),/timeout/i);
  assert.match(d.describe({kind:'disconnect',source:'polar',a:0x216,b:1}),/intentional.*band change/i);
});
test('measurement gaps are not labelled proven Bluetooth disconnections',()=>{
  const d=load();assert.match(d.describe({kind:'gap_recovered',source:'bike',a:5200,b:1}),/5.2 s/);
  assert.doesNotMatch(d.describe({kind:'gap_recovered',source:'bike',a:5200,b:1}),/disconnected/);
});
test('saved report age distinguishes a previous boot and failed checkpoint',()=>{
  const d=load();assert.match(d.savedText({saved_available:true,saved_this_boot:false,save_status:'saved_previous_boot'}),/previous power-on/);
  assert.match(d.savedText({saved_available:true,saved_this_boot:true,save_age_ms:300000,save_status:'write_failed'}),/failed.*5 min/i);
  assert.match(d.savedText({saved_available:false,save_status:'not_saved'}),/No saved report/);
});
