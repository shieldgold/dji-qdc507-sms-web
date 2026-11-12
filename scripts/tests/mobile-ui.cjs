const test=require('node:test');const assert=require('node:assert/strict');const fs=require('node:fs');const path=require('node:path');const {JSDOM}=require('jsdom');
const root=path.resolve(__dirname,'../..'),html=fs.readFileSync(path.join(root,'web/index.html'),'utf8');
const tick=async()=>{for(let i=0;i<4;i++)await new Promise(r=>setImmediate(r));};
function page(overrides={},saved,hash=''){
 const dom=new JSDOM(html,{url:'http://module.test/'+hash,runScripts:'outside-only',pretendToBeVisual:true}),w=dom.window;
 const calls=[],intervals=[];w.setInterval=f=>{intervals.push(f);return intervals.length;};
 const defaults={
  '/api/auth/status':{registered:true,authenticated:true,iccid:'test-card',sim:{state:'READY',blocked:false}},
  '/api/state':{messages:[{key:'in-one',sender:'10010',body:'收到的短信',direction:'in',time:'2026-11-12T12:00:00Z'}],sim:{state:'READY',blocked:false}},
  '/api/overview':{network:{},traffic:{},device:{}}
 };
 if(saved)w.localStorage.setItem('module-v2:test-card',JSON.stringify(saved));
 w.fetch=async(url,options)=>{
  calls.push({url,body:options.body?JSON.parse(options.body):null});
  let response=overrides[url]??overrides[url.split('?')[0]]??defaults[url];
  if(typeof response==='function')response=await response(options);
  if(response===undefined)response={status:404,data:{error:'not found'}};
  const status=typeof response.status==='number'?response.status:200,data=response.data||response;
  return {ok:status<400,status,json:async()=>data};
 };
 w.eval(fs.readFileSync(path.join(root,'web/model.js'),'utf8'));w.eval(fs.readFileSync(path.join(root,'web/app.js'),'utf8'));
 const el=id=>w.document.getElementById(id),click=id=>el(id).click();
 const input=(id,value)=>{el(id).value=value;el(id).dispatchEvent(new w.Event('input',{bubbles:true}));};
 const submit=id=>el(id).dispatchEvent(new w.Event('submit',{bubbles:true,cancelable:true}));
 return {dom,w,calls,intervals,el,click,input,submit,close:()=>w.close()};
}
test('mobile composer separates recipient and keeps a rejected SMS draft',async t=>{
 const p=page({'/api/send':{status:400,data:{error:'SIM 尚未就绪'}}});t.after(p.close);await tick();
 p.click('new-message');p.input('recipient','10010');p.input('message-text','这是一条测试短信');
 assert.equal(p.el('send').disabled,false);assert.equal(p.el('recipient-row').hidden,false);
 p.submit('composer');await tick();assert.equal(p.el('message-text').value,'这是一条测试短信');assert.equal(p.el('send').disabled,false);
 const saved=JSON.parse(p.w.localStorage.getItem('module-v2:test-card'));assert.equal(saved.drafts.__new.text,'这是一条测试短信');assert.equal(saved.pending,null);
});
test('double submit is sent once and a queued SMS clears only its accepted draft',async t=>{
 let state='sending',count=0;
 const message={key:'out:test',sender:'10010',body:'hello',direction:'out',time:'2026-11-12T12:00:00Z',parts:1};
 const p=page({'/api/send':()=>{count++;return {...message,send_state:'queued'};},'/api/send/status':()=>({...message,send_state:state,status:state==='submitted'?'已提交，送达未确认':'正在提交'})});t.after(p.close);await tick();
 p.click('new-message');p.input('recipient','10010');p.input('message-text','hello');p.submit('composer');p.submit('composer');await tick();
 assert.equal(count,1);assert.equal(p.el('message-text').value,'');assert.equal(p.el('send').disabled,true);assert.equal(p.el('recipient-row').hidden,true);
 state='submitted';p.click('check-send');await tick();assert.equal(JSON.parse(p.w.localStorage.getItem('module-v2:test-card')).pending,null);
 assert.match(p.el('conversation').textContent,/送达未确认/);
});
test('lost response retains request tracking and never automatically resends',async t=>{
 let sends=0;const p=page({'/api/send':()=>{sends++;throw new Error('connection lost');}});t.after(p.close);await tick();
 p.click('new-message');p.input('recipient','10010');p.input('message-text','keep me');p.submit('composer');await tick();p.click('check-send');await tick();p.click('check-send');await tick();
 assert.equal(sends,1);assert.equal(p.el('message-text').value,'keep me');assert.equal(p.el('send').disabled,true);assert.equal(p.el('dismiss-send').hidden,false);
});
test('request tracking survives page reload and only performs status reads',async t=>{
 const pending={request_id:'sms-pending-0001',number:'10010',text:'original'};
 const p=page({}, {pending,drafts:{__new:{number:'10010',text:'original'}}});t.after(p.close);await tick();
 assert.equal(p.calls.filter(c=>c.url==='/api/send').length,0);assert.equal(p.calls.filter(c=>c.url.startsWith('/api/send/status')).length,1);
 p.click('new-message');assert.equal(p.el('message-text').value,'original');assert.equal(p.el('send').disabled,true);
});
test('PIN guard allows editing but never submits without explicit retry',async t=>{
 const p=page({'/api/auth/status':{registered:true,authenticated:true,iccid:'test-card',sim:{state:'SIM PIN',blocked:true}}});t.after(p.close);await tick();
 assert.equal(p.el('pin').hidden,false);assert.equal(p.el('pin-value').disabled,false);assert.equal(p.el('btn-pin-unlock').disabled,true);
 p.input('pin-value','1234');p.submit('pin-form');await tick();assert.equal(p.calls.filter(c=>c.url==='/api/sim/unlock').length,0);
});
test('PIN uncertainty clears secret and keeps submit disabled',async t=>{
 const p=page({'/api/auth/status':{registered:true,authenticated:true,iccid:'test-card',sim:{state:'SIM PIN',blocked:false}},'/api/sim/unlock':()=>{throw new Error('lost');}});t.after(p.close);await tick();
 p.input('pin-value','1234');p.submit('pin-form');p.submit('pin-form');await tick();assert.equal(p.calls.filter(c=>c.url==='/api/sim/unlock').length,1);assert.equal(p.el('pin-value').value,'');assert.equal(p.el('btn-pin-unlock').disabled,true);
});
test('message bodies stay text instead of becoming executable HTML',async t=>{
 const attack='<img src=x onerror="window.injected=true">';const p=page({'/api/state':{sim:{state:'READY'},messages:[{key:"x');evil",sender:'10010',body:attack,direction:'in',time:'2026-11-12T12:00:00Z'}]}});t.after(p.close);await tick();
 p.w.document.querySelector('[data-number]').click();assert.equal(p.el('conversation').querySelectorAll('img').length,0);assert.match(p.el('conversation').textContent,/<img/);
});
test('login identifies the external SIM without presenting its unverified phone record as truth',async t=>{
 const p=page({'/api/auth/status':{registered:true,authenticated:false,iccid:'8900000000000001234',number:'+8613800000000',sim:{state:'READY'}}});t.after(p.close);await tick();
 assert.equal(p.el('sim-login').textContent,'模块 SIM · 卡尾号 1234');
 assert.doesNotMatch(p.el('login').textContent,/13800000000/);
 p.input('pw','synthetic-password');p.submit('login-form');await tick();
 assert.equal(p.calls.find(c=>c.url==='/api/auth/login').body.expected_iccid,'8900000000000001234');
});
test('Safari handoff is redeemed once then removed from the address before any request',async t=>{
 const ticket='a'.repeat(64);let loggedIn=false;const p=page({
  '/api/auth/browser/redeem':async()=>{await Promise.resolve();assert.equal(p.w.location.hash,'');loggedIn=true;return {ok:true};},
  '/api/auth/status':()=>({registered:true,authenticated:loggedIn,iccid:'test-card',sim:{state:'READY'}})
 },null,'#browser-login='+ticket);t.after(p.close);await tick();
 assert.equal(p.calls[0].url,'/api/auth/browser/redeem');assert.equal(p.calls[0].body.ticket,ticket);
 assert.equal(p.el('main').hidden,false);p.click('reconnect');await tick();
 assert.equal(p.calls.filter(c=>c.url==='/api/auth/browser/redeem').length,1);
 assert.doesNotMatch(p.w.localStorage.getItem('module-v2:test-card')||'',new RegExp(ticket));
});
test('an expired Safari handoff shows a useful error and never retries automatically',async t=>{
 const p=page({'/api/auth/browser/redeem':{status:401,data:{error:'免密链接已过期，请返回 App 重新打开'}},'/api/auth/status':{registered:true,authenticated:false,iccid:'test-card',sim:{state:'READY'}}},null,'#browser-login='+'b'.repeat(64));t.after(p.close);await tick();
 assert.equal(p.w.location.hash,'');assert.equal(p.el('login').hidden,false);assert.match(p.el('err-login').textContent,/过期/);
 p.click('reconnect');await tick();assert.equal(p.calls.filter(c=>c.url==='/api/auth/browser/redeem').length,1);
});
