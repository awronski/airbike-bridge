const assert=require('node:assert/strict');
const {test}=require('node:test');
const {readFileSync}=require('node:fs');
const {join}=require('node:path');
const vm=require('node:vm');

function dashboard(points,maxHR=190,rider=1){
  const html=readFileSync(join(__dirname,'../web/index.html'),'utf8');
  const source=html.slice(html.indexOf('const zoneColors='),html.indexOf('new ResizeObserver'));
  let path=[],strokes=[],arcs=0;
  const ctx={font:'',fillStyle:'',strokeStyle:'',globalAlpha:1,
    beginPath(){path=[];},moveTo(x,y){path.push(['move',x,y]);},lineTo(x,y){path.push(['line',x,y]);},
    stroke(){strokes.push({color:this.strokeStyle,path:[...path]});},arc(){arcs++;},
    scale(){},fillText(){},fill(){},save(){},restore(){},rect(){},clip(){},closePath(){},translate(){},rotate(){},measureText(){return {width:7};}
  };
  const nodes={'hr-chart':{getBoundingClientRect:()=>({width:350,height:115}),getContext:()=>ctx,setAttribute(){}},'trend-note':{}};
  const context=vm.createContext({el:id=>nodes[id],text:(id,value)=>nodes[id].textContent=value,
    innerWidth:390,window:{devicePixelRatio:1},performance:{now:()=>0},rideData:{selection:{ready:true,rider_id:rider}},
    document:{querySelector(){return {dataset:{},style:{setProperty(){}}};}}});
  vm.runInContext(source,context);
  context.fixture={points,rider_id:1,ready:true,interval_s:5};context.maximum=maxHR;
  vm.runInContext('trendData=fixture;focusMaxHR=maximum;drawTrend();',context);
  return {context,strokes,arcs,nodes};
}
test('personal zones use saved maximum HR and exact boundaries',()=>{
  const {context}=dashboard([]);
  for(const [bpm,zone] of [[50,0],[113,0],[114,1],[132,1],[133,2],[151,2],[152,3],[170,3],[171,4],[250,4]]){
    context.bpm=bpm;assert.equal(vm.runInContext('zoneFor(bpm)',context),zone);
  }
  assert.equal(vm.runInContext('zoneRange(1)',context),'114 – 132 bpm');
  vm.runInContext('focusMaxHR=null',context);
  assert.equal(vm.runInContext('zoneFor(125)',context),null);
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
