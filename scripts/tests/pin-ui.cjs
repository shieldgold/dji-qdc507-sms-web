const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('ui.go', 'utf8').match(/<script>([\s\S]*?)<\/script>/)[1];
const flush = () => new Promise(resolve => setImmediate(resolve));
function page(status, unlock) {
  const elements = new Map();
  const requests = [];
  const el = id => {
    if (!elements.has(id)) elements.set(id, {style:{},value:'',checked:false,disabled:false,
      textContent:'',innerHTML:'',classList:{add(){},remove(){}},focus(){},addEventListener(){}});
    return elements.get(id);
  };
  const context = vm.createContext({
    document:{getElementById:el,cookie:''}, window:{scrollTo(){}}, navigator:{},
    setInterval(){},setTimeout(){},confirm(){return true},
    fetch:async (url, options) => {
      if (url === 'api/auth/status') return {status:200,json:async()=>({...status})};
      if (url === 'api/state') return {status:200,json:async()=>({sim:status.sim,messages:[]})};
      requests.push({url,body:JSON.parse(options.body)});
      const data = await unlock(url, options);
      return {status:200,json:async()=>data};
    }
  });
  vm.runInContext(source,context);
  return {el,requests,context};
}
(async () => {
  const setup=page({registered:false,sim:{state:'SIM PIN',blocked:false}},()=>({}));
  await flush();
  assert.equal(setup.el('setup').style.display,'');
  assert.equal(setup.el('pin').style.display,'none');
  const status={registered:true,authenticated:true,sim:{state:'SIM PIN',blocked:false}};
  let finish;
  const p=page(status,()=>new Promise(resolve=>{finish=resolve}));
  await flush();
  assert.equal(p.el('pin').style.display,'');
  assert.equal(p.el('btn-pin-unlock').disabled,false);
  p.el('pin-value').value='bad';
  p.el('btn-pin-unlock').onclick();
  assert.equal(p.requests.length,0);
  p.el('pin-value').value='1234';
  p.el('btn-pin-unlock').onclick();
  p.el('btn-pin-unlock').onclick();
  assert.equal(p.requests.length,1);
  assert.equal(p.el('btn-pin-unlock').disabled,true);
  finish({error:'未确认成功',sim:{state:'SIM PIN',blocked:true}});
  await flush();
  assert.equal(p.el('pin-value').value,'');
  assert.equal(p.el('btn-pin-unlock').disabled,true);
  assert.equal(p.el('btn-pin-retry').style.display,'');
  const q=page(status,async()=>{throw new Error('network lost')});
  await flush();
  q.el('pin-value').value='1234';
  q.el('btn-pin-unlock').onclick();
  await flush();
  assert.equal(q.el('pin-value').value,'');
  assert.equal(q.el('btn-pin-unlock').disabled,true);
  assert.match(q.el('err-pin').textContent,/先刷新/);
  const locked=page({...status,sim:{state:'SIM PUK',blocked:false}},()=>({}));
  await flush();
  assert.equal(locked.el('btn-pin-unlock').disabled,true);
  const ready=page({...status,sim:{state:'READY',blocked:false}},()=>({}));
  await flush();
  assert.equal(ready.el('main').style.display,'');
  console.log('PIN UI: first setup, locked SIM, invalid input, duplicate submission, uncertain result, PUK and READY passed');
})().catch(e=>{console.error(e);process.exitCode=1});
