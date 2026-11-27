const assert=require('node:assert/strict');
const {test}=require('node:test');
const {readFileSync}=require('node:fs');
const {join}=require('node:path');
const vm=require('node:vm');

function dashboard(points,maxHR=190,rider=1,active=true){
  const html=readFileSync(join(__dirname,'../web/index.html'),'utf8');
  const source=html.slice(html.indexOf('const zoneColors='),html.indexOf('new ResizeObserver'));
  let path=[],strokes=[],labels=[],arcs=0;
  const ctx={font:'',fillStyle:'',strokeStyle:'',globalAlpha:1,
    beginPath(){path=[];},moveTo(x,y){path.push(['move',x,y]);},lineTo(x,y){path.push(['line',x,y]);},
    stroke(){strokes.push({color:this.strokeStyle,path:[...path]});},arc(){arcs++;},
    scale(){},fillText(value){labels.push({value,color:this.fillStyle});},fill(){},save(){},restore(){},rect(){},clip(){},closePath(){},measureText(value){return {width:String(value).length*7};}
  };
  const reading=()=>({classList:{toggle(){}},style:{}});
  const nodes={
    'hr-chart':{getBoundingClientRect:()=>({width:350,height:144}),getContext:()=>ctx},'trend-note':{},
    'trend-toggle':{addEventListener(_event,fn){this.tap=fn;},setAttribute(_attr,value){this.label=value;}},
    'hr':{},'hr-zone':{},'power':{},'hr-value':reading(),'power-value':reading()
  };
  const context=vm.createContext({el:id=>nodes[id],text:(id,value)=>nodes[id].textContent=value,
    innerWidth:390,window:{devicePixelRatio:1},performance:{now:()=>0},rideData:{active,selection:{ready:true,rider_id:rider}},
    document:{querySelector(){return {dataset:{},style:{setProperty(){}}};}}});
  vm.runInContext(source,context);
  context.fixture={points,rider_id:1,ready:true,interval_s:5};context.maximum=maxHR;
  vm.runInContext('trendData=fixture;focusMaxHR=maximum;drawTrend();',context);
  return {context,strokes,labels,arcs,nodes};
}
test('personal zones use saved maximum HR and exact boundaries',()=>{
  const {context}=dashboard([]);
  for(const [bpm,zone] of [[50,0],[113,0],[114,1],[132,1],[133,2],[151,2],[152,3],[170,3],[171,4],[250,4]]){
    context.bpm=bpm;assert.equal(vm.runInContext('zoneFor(bpm)',context),zone);
  }
  vm.runInContext('renderHeartRate(134)',context);
  vm.runInContext('focusMaxHR=null',context);
  assert.equal(vm.runInContext('zoneFor(125)',context),null);
});
test('power changes color at all six Garmin cycling thresholds and preserves zero',()=>{
  const {context,nodes}=dashboard([]);
  for(const [watts,zone] of [[0,0],[110,0],[111,1],[150,1],[151,2],[180,2],[181,3],[210,3],
    [211,4],[240,4],[241,5],[300,5],[301,6]]){
    assert.equal(vm.runInContext('powerZoneFor('+watts+',200)',context),zone);
  }
  vm.runInContext('renderPower(0,200);renderHeartRate(134)',context);
  assert.equal(nodes.power.textContent,0);
  assert.equal(nodes['power-value'].style.color,'#9d9d9d');
  assert.equal(nodes['hr-value'].style.color,'#9efb32');
  vm.runInContext('renderPower(140,null)',context);
  assert.equal(nodes['power-value'].style.color,'#fafafa');
  vm.runInContext('renderPower(null,200)',context);
  assert.equal(nodes.power.textContent,'—');
});
test('the zone label follows the live HR color and clears with missing data',()=>{
  const {context,nodes}=dashboard([]);
  vm.runInContext('renderHeartRate(126)',context);
  assert.equal(nodes['hr-zone'].textContent,' · Zone 2');
  assert.equal(nodes['hr-value'].style.color,'#00a5ff');
  vm.runInContext('renderHeartRate(134)',context);
  assert.equal(nodes['hr-zone'].textContent,' · Zone 3');
  vm.runInContext('renderHeartRate(null)',context);
  assert.equal(nodes['hr-zone'].textContent,'');
  vm.runInContext('focusMaxHR=null;renderHeartRate(126)',context);
  assert.equal(nodes.hr.textContent,126);
  assert.equal(nodes['hr-zone'].textContent,'');
});
test('live chart draws real HR/power on separate scales without endpoint dots',()=>{
  const {strokes,arcs}=dashboard([[10,125,100],[5,130,200]]);
  const hr=strokes.find(p=>p.color==='#00a5ff');
  const power=strokes.find(p=>p.color==='#d2bbff');
  assert.ok(hr&&power);assert.equal(arcs,0);
  assert.equal(power.path.filter(p=>p[0]==='line').length,1);
  assert.notEqual(hr.path[0][2],power.path[0][2]);
});
test('no fictional data, no connection through gaps, and rider isolation',()=>{
  for(const points of [[],[[15,null,null],[10,null,null]],[[20,120,150],[5,130,200]]]){
    const {strokes}=dashboard(points);
    assert.ok(!strokes.some(s=>['#00a5ff','#d2bbff'].includes(s.color)&&s.path.some(p=>p[0]==='line')));
  }
  const foreign=dashboard([[10,125,100],[5,130,200]],190,2);
  assert.ok(!foreign.strokes.some(p=>p.color==='#00a5ff'));
});
test('zero watts is valid and power continues when HR is missing',()=>{
  const {strokes}=dashboard([[10,null,0],[5,null,0]],null);
  const power=strokes.find(p=>p.color==='#d2bbff');
  assert.ok(power.path.some(p=>p[0]==='line'));
  assert.ok(!strokes.some(p=>p.color==='#00a5ff'));
});
test('before Start the chart only draws power while the live BPM can still be shown',()=>{
  const {context,strokes,labels,nodes}=dashboard([[10,125,100],[5,130,200]],190,1,false);
  assert.ok(!strokes.some(p=>p.color==='#00a5ff'));
  assert.ok(!labels.some(t=>t.value==='HR · bpm'));
  assert.ok(strokes.some(p=>p.color==='#d2bbff'&&p.path.some(point=>point[0]==='line')));
  assert.match(nodes['trend-toggle'].label,/^Power chart/);
  vm.runInContext('renderHeartRate(125)',context);
  assert.equal(nodes.hr.textContent,125);
});
test('chart starts at 0–200 W without readings',()=>{
  const {labels}=dashboard([]);
  const watts=labels.filter(t=>t.color==='#d2bbff'&&typeof t.value==='number').map(t=>t.value);
  assert.deepEqual(watts,[0,50,100,150,200]);
});
test('the chart opens at 15 minutes and toggles the window and power scale',()=>{
  const {labels,nodes}=dashboard([[1500,135,343],[1495,135,350],[15,125,140],[10,126,160]]);
  function axis(){return labels.filter(t=>t.color==='#d2bbff'&&typeof t.value==='number').map(t=>t.value);}
  assert.deepEqual(axis(),[0,50,100,150,200]);
  assert.ok(labels.some(t=>t.value==='-15:00')&&labels.some(t=>t.value==='-5:00'));
  assert.match(nodes['trend-toggle'].label,/last 15 minutes.*30 minutes/);
  assert.equal(nodes['trend-note'].textContent,'Last 15 minutes');
  labels.length=0;
  nodes['trend-toggle'].tap();
  assert.deepEqual(axis(),[0,100,200,300,400]);
  assert.ok(labels.some(t=>t.value==='-30:00'));
  assert.match(nodes['trend-toggle'].label,/last 30 minutes.*15 minutes/);
  assert.equal(nodes['trend-note'].textContent,'Last 30 minutes');
  labels.length=0;
  nodes['trend-toggle'].tap();
  assert.deepEqual(axis(),[0,50,100,150,200]);
  assert.equal(nodes['trend-note'].textContent,'Last 15 minutes');
});
