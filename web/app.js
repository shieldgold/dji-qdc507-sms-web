(function(){
'use strict';
const $=id=>document.getElementById(id);
const S={sim:null,storageKey:'',messages:[],page:'messages',number:'',newMessage:false,query:'',favoriteOnly:false,seen:{},favorites:{},drafts:{},pending:null,loading:false,pendingChecking:false,pendingMisses:0,online:true,threadSignature:'',overview:null};
let toastTimer,pollTimer,overviewTimer,pinSending=false,confirmResolve=null;
function text(id,value){$(id).textContent=value==null?'':String(value);}
function error(id,value){text(id,value);$(id).hidden=!value;}
function toast(value){text('toast',value);$('toast').classList.add('on');clearTimeout(toastTimer);toastTimer=setTimeout(()=>$('toast').classList.remove('on'),3600);}
function node(tag,className,value){const e=document.createElement(tag);if(className)e.className=className;if(value!=null)e.textContent=value;return e;}
function storeRead(key,fallback){try{return JSON.parse(localStorage.getItem(key))||fallback;}catch(_){return fallback;}}
function saveLocal(){if(!S.storageKey)return;try{localStorage.setItem(S.storageKey,JSON.stringify({seen:S.seen,favorites:S.favorites,drafts:S.drafts,pending:S.pending}));}catch(_){text('draft-status','此浏览器无法保存草稿');}}
function loadLocal(sim){
 const key='module-v2:'+String(sim.iccid||sim.imsi||'local');if(key===S.storageKey)return;
 S.storageKey=key;const saved=storeRead(key,{});S.seen=saved.seen||{};S.favorites=saved.favorites||{};S.drafts=saved.drafts||{};S.pending=saved.pending||null;
}
function connection(ok){S.online=ok;$('connection-banner').hidden=ok;if(!ok)text('connection-text','模块连接中断，草稿已保留');}
async function api(path,body,timeout=20000){
 const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),timeout);
 try{
  const options={cache:'no-store',signal:controller.signal};if(body!==undefined){options.method='POST';options.headers={'Content-Type':'application/json'};options.body=JSON.stringify(body);}
  const response=await fetch(path,options);const data=await response.json();
  if(!response.ok){const e=new Error(data.error||'请求未完成');e.status=response.status;e.data=data;throw e;}
  connection(true);return data;
 }catch(e){if(!e.status)connection(false);throw e;}finally{clearTimeout(timer);}
}
function showGate(name){['boot-screen','setup','login','pin','chpw','main'].forEach(id=>$(id).hidden=id!==name);}
function simLabel(id,sim){text(id,sim.iccid?'模块 SIM · 卡尾号 '+String(sim.iccid).slice(-4):'模块 SIM · 尚未识别卡号');}
let browserHandoffError='';
async function receiveBrowserLogin(){
 if(!location.hash.startsWith('#browser-login='))return;
 const ticket=location.hash.slice('#browser-login='.length);
 // A fragment is not sent to the server; remove it before any other request.
 history.replaceState(history.state,'',location.pathname+location.search);
 try{await api('/api/auth/browser/redeem',{ticket});}catch(e){browserHandoffError=e.status?e.message:'免密登录未完成，请返回 App 重新打开 Safari。';}
}
async function boot(){
 try{
  const st=await api('/api/auth/status');S.sim=st;loadLocal(st);simLabel('sim-setup',st);simLabel('sim-login',st);
  if(!st.registered){showGate('setup');return;}
  if(!st.authenticated){showGate('login');error('err-login',browserHandoffError);return;}
  try{await api('/api/time',{unix_ms:Date.now()});}catch(_){}
  if(st.sim&&st.sim.state!=='READY'){showPIN(st.sim);return;}
  showGate('main');showPage(S.page==='thread'?'messages':S.page);await refreshMessages();
  if(S.pending){checkPending();}
 }catch(e){showGate('boot-screen');error('boot-error','暂时无法连接模块。请检查 USB 连接，等待模块启动后重试。');$('boot-retry').hidden=false;}
}
function showPIN(st){
 showGate('pin');const labels={'SIM PIN':'SIM 已锁定，请输入 PIN','SIM PUK':'SIM 需要 PUK，请在手机的 SIM 管理中处理','READY':'SIM 已就绪'};
 text('pin-state',st.blocked?'上次解锁结果未确认。请先刷新状态，核实 PIN 后才能重试。':labels[st.state]||('SIM 状态：'+(st.state||'未知')));
 $('pin-value').disabled=pinSending||st.state!=='SIM PIN';$('btn-pin-unlock').disabled=pinSending||st.blocked||st.state!=='SIM PIN';$('btn-pin-retry').hidden=!(st.blocked&&st.state==='SIM PIN');
}
function ask(title,detail,label='确认'){
 text('confirm-title',title);text('confirm-detail',detail);text('confirm-ok',label);
 if(typeof $('confirm-dialog').showModal!=='function')return Promise.resolve(window.confirm(title+'\n'+detail));
 $('confirm-dialog').showModal();return new Promise(resolve=>{confirmResolve=resolve;});
}
function answer(value){$('confirm-dialog').close();if(confirmResolve){confirmResolve(value);confirmResolve=null;}}
$('confirm-ok').addEventListener('click',()=>answer(true));$('confirm-cancel').addEventListener('click',()=>answer(false));$('confirm-dialog').addEventListener('cancel',e=>{e.preventDefault();answer(false);});
$('setup-form').addEventListener('submit',async e=>{
 e.preventDefault();const password=$('pw1').value,confirm=$('pw2').value;error('err-setup','');
 if(Array.from(password).length<4){error('err-setup','密码至少 4 位');return;}if(password!==confirm){error('err-setup','两次输入的密码不一致');return;}
 $('btn-setup').disabled=true;
 try{await api('/api/auth/setup',{password,confirm,remember:$('rm-setup').checked});$('pw1').value=$('pw2').value='';await boot();}catch(err){error('err-setup',err.message);}finally{$('btn-setup').disabled=false;}
});
$('login-form').addEventListener('submit',async e=>{
 e.preventDefault();error('err-login','');$('btn-login').disabled=true;
 try{await api('/api/auth/login',{password:$('pw').value,remember:$('rm-login').checked,expected_iccid:S.sim.iccid||''});$('pw').value='';browserHandoffError='';await boot();}catch(err){error('err-login',err.message);}finally{$('btn-login').disabled=false;}
});
$('pin-form').addEventListener('submit',async e=>{
 e.preventDefault();if(pinSending||$('btn-pin-unlock').disabled)return;
 const pin=$('pin-value').value;if(!/^[0-9]{4,8}$/.test(pin)){error('err-pin','PIN 必须为 4–8 位数字');return;}
 pinSending=true;$('btn-pin-unlock').disabled=true;text('btn-pin-unlock','正在解锁…');error('err-pin','');
 try{await api('/api/sim/unlock',{pin,request_id:requestID()});await boot();}
 catch(err){if(err.status===401){await boot();}else{if(err.data&&err.data.sim){showPIN(err.data.sim);}error('err-pin',err.status?err.message:'连接中断，解锁结果待确认。请先刷新状态，不要直接重试。');}}
 finally{$('pin-value').value='';pinSending=false;text('btn-pin-unlock','解锁一次');}
});
$('btn-pin-refresh').addEventListener('click',()=>{error('err-pin','');boot();});
$('btn-pin-retry').addEventListener('click',async()=>{
 if(!await ask('允许再试一次？','请先确认已核实正确 PIN。再次输入错误可能锁卡。','允许一次'))return;
 try{const st=await api('/api/sim/retry',{confirmed:true});error('err-pin','');showPIN(st);}catch(e){error('err-pin',e.message);}
});
$('boot-retry').addEventListener('click',boot);$('reconnect').addEventListener('click',boot);
$('btn-chpw').addEventListener('click',()=>showGate('chpw'));$('btn-chpw-back').addEventListener('click',()=>{showGate('main');showPage('device');});
$('password-form').addEventListener('submit',async e=>{
 e.preventDefault();error('err-chpw','');const password=$('newpw1').value,confirm=$('newpw2').value;
 if(password!==confirm){error('err-chpw','两次新密码不一致');return;}$('btn-chpw-save').disabled=true;
 try{await api('/api/auth/change',{old:$('oldpw').value,password,confirm,remember:true});$('oldpw').value=$('newpw1').value=$('newpw2').value='';showGate('main');showPage('device');toast('访问密码已修改');}catch(err){error('err-chpw',err.message);}finally{$('btn-chpw-save').disabled=false;}
});
$('btn-logout').addEventListener('click',async()=>{saveDraft();try{await api('/api/auth/logout',{});await boot();}catch(e){toast(e.message);}});
function showPage(name){
 S.page=name;['messages','thread','network','device'].forEach(p=>$(p+'-page').hidden=p!==name);$('bottom-nav').hidden=name==='thread';
 document.querySelectorAll('[data-page]').forEach(b=>{b.classList.toggle('active',b.dataset.page===name);if(b.dataset.page===name)b.setAttribute('aria-current','page');else b.removeAttribute('aria-current');});
 if(name==='messages')renderThreads();if(name==='network'||name==='device')refreshOverview();
 resizeViewport();
}
$('bottom-nav').addEventListener('click',e=>{const b=e.target.closest('[data-page]');if(b){saveDraft();showPage(b.dataset.page);}});
function timeLabel(value,full=false){const t=SMS.timestamp(value);if(!t)return value||'';const d=new Date(t);if(full)return d.toLocaleString('zh-CN',{month:'numeric',day:'numeric',hour:'2-digit',minute:'2-digit',hour12:false});return d.toLocaleDateString()===new Date().toLocaleDateString()?d.toLocaleTimeString('zh-CN',{hour:'2-digit',minute:'2-digit',hour12:false}):d.toLocaleDateString('zh-CN',{month:'numeric',day:'numeric'});}
function renderThreads(){
 const rows=SMS.threads(S.messages,S.query,S.seen,S.favorites).filter(t=>!S.favoriteOnly||t.favorite);const list=$('thread-list');list.replaceChildren();text('message-summary',rows.length+' 个会话');
 if(!rows.length){const empty=node('div','empty-state');empty.append(node('strong','',S.query?'没有匹配的短信':S.favoriteOnly?'还没有收藏':'还没有短信'),node('p','',S.query?'试试其他号码或关键词':S.favoriteOnly?'在会话里收藏需要保留的短信':'新短信会自动出现在这里，也可以先发送一条。'));list.append(empty);return;}
 for(const t of rows){
  const row=node('button','thread-row');row.dataset.number=t.number;row.setAttribute('aria-label','打开与 '+t.number+' 的会话');
  row.append(node('span','avatar',t.number.slice(-2)));const info=node('div','thread-info'),top=node('div','thread-top');top.append(node('span','thread-name',t.number),node('span','thread-date',timeLabel(t.latest.time)));if(t.unread)top.append(node('span','unread',t.unread>99?'99+':t.unread));
  info.append(top,node('p','thread-preview',(t.latest.direction==='out'?'我：':'')+(t.latest.body||'')));row.append(info);list.append(row);
 }
}
$('thread-list').addEventListener('click',e=>{const b=e.target.closest('[data-number]');if(b)openThread(b.dataset.number);});
$('search').addEventListener('input',()=>{S.query=$('search').value;renderThreads();});
function setFilter(favorite){S.favoriteOnly=favorite;$('filter-all').classList.toggle('selected',!favorite);$('filter-favorite').classList.toggle('selected',favorite);$('filter-all').setAttribute('aria-pressed',String(!favorite));$('filter-favorite').setAttribute('aria-pressed',String(favorite));renderThreads();}
$('filter-all').addEventListener('click',()=>setFilter(false));$('filter-favorite').addEventListener('click',()=>setFilter(true));
function currentNumber(){return S.newMessage?SMS.normalizeNumber($('recipient').value):S.number;}
function draftKey(){return S.newMessage?'__new':S.number;}
function saveDraft(){if(S.page!=='thread')return;const value=$('message-text').value,key=draftKey();if(value||S.newMessage&&$('recipient').value){S.drafts[key]={text:value,number:currentNumber()};}else delete S.drafts[key];saveLocal();}
function openThread(number,isNew=false){
 saveDraft();S.number=number;S.newMessage=isNew;S.threadSignature='';
 const draft=S.drafts[isNew?'__new':number]||{};$('recipient').value=draft.number||number;$('recipient-row').hidden=!isNew;$('message-text').value=draft.text||'';
 text('thread-title',isNew?'新短信':number);text('thread-subtitle',isNew?'填写收件人，然后输入内容':'普通短信 · 通过模块发送');
 if(S.page!=='thread')history.pushState({thread:true},'');showPage('thread');markSeen();renderConversation(true);updateComposer();
 if(isNew)$('recipient').focus();
}
function markSeen(){if(S.newMessage)return;for(const m of S.messages){if(SMS.normalizeNumber(m.sender)===S.number)S.seen[m.key]=true;}saveLocal();}
function goBack(){saveDraft();$('message-text').blur();$('recipient').blur();if(history.state&&history.state.thread)history.back();else showPage('messages');}
$('new-message').addEventListener('click',()=>openThread('',true));$('thread-back').addEventListener('click',goBack);window.addEventListener('popstate',()=>{if(S.page==='thread'){saveDraft();showPage('messages');}});
function renderConversation(forceBottom=false){
 if(S.page!=='thread')return;
 const number=currentNumber(),messages=S.messages.filter(m=>SMS.normalizeNumber(m.sender)===number).sort((a,b)=>SMS.timestamp(a.time)-SMS.timestamp(b.time));
 const signature=JSON.stringify([number,messages.map(m=>[m.key,m.send_state,m.sent_parts,m.status,m.send_error,!!S.favorites[m.key]])]);
 if(signature===S.threadSignature&&!forceBottom)return;S.threadSignature=signature;
 const area=$('conversation'),atBottom=area.scrollHeight-area.scrollTop-area.clientHeight<90;const oldScroll=area.scrollTop;area.replaceChildren();
 if(!messages.length){const empty=node('div','empty-state');empty.append(node('strong','',S.newMessage?'开始一段对话':'还没有历史短信'),node('p','','发送前可查看短信条数，发送结果会显示在会话中。'));area.append(empty);}
 for(const m of messages){
  const group=node('article','message-group'+(m.direction==='out'?' out':'')),bubble=node('div','message-bubble');bubble.append(node('span','',m.body||''));
  if(m.code&&m.direction!=='out'){const copy=node('button','copy-code','复制 '+m.code);copy.dataset.copy=m.code;bubble.append(copy);}
  group.append(bubble);const meta=node('div','message-meta');meta.append(node('time','',timeLabel(m.time,true)));if(m.direction==='out')meta.append(node('span','',m.status||'历史发送记录'));group.append(meta);
  if(m.send_error)group.append(node('p','send-error',m.send_error));
  const actions=node('div','message-actions');const fav=node('button',S.favorites[m.key]?'is-favorite':'',S.favorites[m.key]?'已收藏':'收藏');fav.dataset.favorite=m.key;fav.setAttribute('aria-pressed',String(!!S.favorites[m.key]));actions.append(fav);
  if(m.direction==='out'&&['failed','partial','uncertain'].includes(m.send_state)){const edit=node('button','','复制到草稿');edit.dataset.draft=m.key;actions.append(edit);}
  group.append(actions);area.append(group);
 }
 if(forceBottom||atBottom)area.scrollTop=area.scrollHeight;else area.scrollTop=oldScroll;
}
async function copyText(value){
 try{if(navigator.clipboard&&navigator.clipboard.writeText){await navigator.clipboard.writeText(value);}else{const t=document.createElement('textarea');t.value=value;t.style.position='fixed';t.style.opacity='0';document.body.append(t);t.select();const ok=document.execCommand('copy');t.remove();if(!ok)throw new Error('copy failed');}toast('已复制');}catch(_){toast('请长按文字复制');}
}
$('conversation').addEventListener('click',async e=>{
 const copy=e.target.closest('[data-copy]');if(copy){copyText(copy.dataset.copy);return;}
 const fav=e.target.closest('[data-favorite]');if(fav){const key=fav.dataset.favorite;if(S.favorites[key])delete S.favorites[key];else S.favorites[key]=true;saveLocal();renderConversation();return;}
 const draft=e.target.closest('[data-draft]');if(draft){const m=S.messages.find(m=>m.key===draft.dataset.draft);if(!m)return;if(!await ask('复制到草稿？','这条短信可能已经部分或全部提交。请核对接收端，避免重复发送。','复制草稿'))return;$('message-text').value=m.body;updateComposer();saveDraft();$('message-text').focus();}
});
function updateComposer(){
 const value=$('message-text').value,count=SMS.segments(value),valid=SMS.validNumber(currentNumber());
 $('message-text').style.height='auto';$('message-text').style.height=Math.min(150,Math.max(46,$('message-text').scrollHeight))+'px';
 text('sms-count',value?Array.from(value).length+' 字 · '+count.count+' 条短信':'草稿仅保存在此设备');$('sms-count').classList.toggle('over-limit',count.count>10);
 $('send').disabled=!!S.pending||!value.trim()||!valid||count.count>10;text('send',S.pending?'提交中…':'发送');
 if(S.newMessage)error('recipient-error',$('recipient').value&&!valid?'请输入有效号码，可带 + 国家区号':'');
 text('draft-status',value?'草稿已保留':'');
}
$('message-text').addEventListener('input',()=>{updateComposer();saveDraft();});$('recipient').addEventListener('input',()=>{updateComposer();saveDraft();renderConversation();});
function requestID(){return 'sms-'+Date.now().toString(36)+'-'+Math.random().toString(36).slice(2)+Math.random().toString(36).slice(2,7);}
function mergeMessage(message){if(!message||!message.key)return;const i=S.messages.findIndex(m=>m.key===message.key);if(i>=0)S.messages[i]=message;else S.messages.push(message);}
function pendingNotice(value,allowDismiss=false){$('send-notice').hidden=!value;text('send-notice-text',value);$('dismiss-send').hidden=!allowDismiss;}
function accepted(message,snapshot){
 mergeMessage(message);const key=snapshot.draft_key||(S.newMessage?'__new':snapshot.number);
 if(S.drafts[key]&&S.drafts[key].text===snapshot.text)delete S.drafts[key];
 if(S.page==='thread'&&currentNumber()===snapshot.number){
  if($('message-text').value===snapshot.text)$('message-text').value='';
  S.number=snapshot.number;S.newMessage=false;$('recipient-row').hidden=true;text('thread-title',snapshot.number);text('thread-subtitle','普通短信 · 通过模块发送');
 }
 saveLocal();renderConversation(true);updateComposer();
}
$('composer').addEventListener('submit',async e=>{
 e.preventDefault();if(S.pending)return;
 const number=currentNumber(),content=$('message-text').value,count=SMS.segments(content);
 if(!SMS.validNumber(number)||!content.trim()||count.count>10){toast('请检查号码和短信内容');return;}
 const snapshot={request_id:requestID(),number,text:content,draft_key:draftKey()};S.pending=snapshot;S.pendingMisses=0;saveLocal();updateComposer();pendingNotice('正在提交短信，请勿重复发送。');
 try{const message=await api('/api/send',snapshot,15000);accepted(message,snapshot);checkPending();}
 catch(err){
  if(err.status&&err.status<500){S.pending=null;saveLocal();pendingNotice('');updateComposer();toast(err.message);if(err.status===401)boot();}
  else{pendingNotice('连接中断，发送结果待确认。草稿已保留，正在检查发送记录。');checkPending();}
 }
});
async function checkPending(){
 if(!S.pending||S.pendingChecking)return;const pending=S.pending;S.pendingChecking=true;
 try{
  const message=await api('/api/send/status?request_id='+encodeURIComponent(pending.request_id));if(S.pending!==pending)return;
  mergeMessage(message);S.pendingMisses=0;
  if(message.send_state==='queued'||message.send_state==='sending'){accepted(message,pending);pendingNotice('正在提交 '+(message.sent_parts||0)+'/'+(message.parts||1)+' 条短信。请等待结果。');}
  else{
   if(message.send_state==='submitted'){accepted(message,pending);pendingNotice('');toast('已提交到网络，送达尚未确认');}
   else{pendingNotice(message.status+'。请先核对接收端，再决定是否重新发送。');toast(message.status||'发送结果待确认');}
   S.pending=null;saveLocal();updateComposer();renderConversation();
  }
 }catch(err){
  if(err.status===404){S.pendingMisses++;pendingNotice('尚未找到发送记录，请先核对接收端，避免重复发送。',S.pendingMisses>=3);}
  else if(err.status===401){showGate('login');}
  else pendingNotice('模块暂时无法连接，发送结果待确认。重新连接后会继续检查。');
 }finally{S.pendingChecking=false;}
}
$('check-send').addEventListener('click',()=>{if(S.pending)checkPending();else refreshMessages();});
$('dismiss-send').addEventListener('click',async()=>{if(!await ask('停止追踪这次发送？','内容会保留为草稿。若准备重新发送，请先确认接收端没有收到。','保留草稿'))return;S.pending=null;saveLocal();pendingNotice('');updateComposer();});
async function refreshMessages(){
 if(S.loading||$('main').hidden)return;S.loading=true;
 try{const data=await api('/api/state');S.messages=data.messages||[];if(data.sim&&data.sim.state!=='READY'){showPIN(data.sim);return;}if(S.page==='thread'){markSeen();renderConversation();}else if(S.page==='messages')renderThreads();}
 catch(err){if(err.status===401){showGate('login');error('err-login','登录已过期，草稿已保留');}}
 finally{S.loading=false;}
}
$('refresh-messages').addEventListener('click',refreshMessages);$('thread-check').addEventListener('click',()=>{refreshMessages();checkPending();});
function bytes(n){n=Number(n)||0;const units=['B','KB','MB','GB','TB'];let i=0;while(n>=1024&&i<units.length-1){n/=1024;i++;}return (i===0?String(Math.round(n)):n.toFixed(n>=100?0:1))+' '+units[i];}
function total(count){return Number(count&&count.upload||0)+Number(count&&count.download||0);}
function uptime(seconds){seconds=Number(seconds)||0;if(seconds>=86400)return Math.floor(seconds/86400)+' 天 '+Math.floor(seconds%86400/3600)+' 小时';if(seconds>=3600)return Math.floor(seconds/3600)+' 小时 '+Math.floor(seconds%3600/60)+' 分钟';return Math.floor(seconds/60)+' 分钟';}
function renderOverview(data){
 const n=data.network||{},t=data.traffic||{},d=data.device||{};
 const carriers={'46001':'中国联通','46006':'中国联通','46009':'中国联通','46000':'中国移动','46002':'中国移动','46007':'中国移动','46003':'中国电信','46005':'中国电信','46011':'中国电信'};
 text('network-carrier',carriers[n.operator]||n.operator||'运营商待识别');text('registration',n.registration||'检查中');$('registration').classList.toggle('good',!!n.registered);
 text('network-tech',n.technology||'等待网络');text('network-band',n.band||'频段暂不可用');text('rsrp',n.rsrp||'—');text('rsrq',n.rsrq||'—');text('sinr',n.sinr||'—');
 text('rate-down',bytes(t.rate&&t.rate.download)+'/s');text('rate-up',bytes(t.rate&&t.rate.upload)+'/s');text('usage-today',t.clock_valid?bytes(total(t.today)):'时间未同步');text('usage-month',t.clock_valid?bytes(total(t.month)):'时间未同步');text('usage-total',bytes(total(t.total)));
 text('network-apn',n.apn||'—');text('network-status',n.error||n.registration||'—');text('network-updated',n.updated_at?timeLabel(n.updated_at,true):'—');
 text('app-version','v'+(d.version||'2.0.0'));text('device-uptime','已运行 '+uptime(d.uptime));text('device-firmware',d.firmware||'读取中');text('device-memory',bytes(d.memory_available)+' / '+bytes(d.memory_total));text('device-storage',bytes(d.disk_available)+' / '+bytes(d.disk_total));$('rollback-version').hidden=!d.rollback_available;
 text('voice-state','IMS 配置：'+(n.ims||'待检测')+'；音频设备：'+(d.audio_ready?'已发现，通话仍需验证':'尚未就绪')+'。请使用“小电波”App，iPhone 直连后的双向声音仍需验证。');
}
async function refreshOverview(){try{const data=await api('/api/overview');S.overview=data;renderOverview(data);}catch(err){if(err.status===401)boot();else toast('网络信息暂时无法刷新');}}
$('refresh-network').addEventListener('click',refreshOverview);
$('run-diagnostics').addEventListener('click',async()=>{
 const btn=$('run-diagnostics');btn.disabled=true;btn.textContent='正在检查…';$('diagnostics').hidden=false;$('diagnostics').replaceChildren(node('p','caption','正在核对 SIM、网络注册和短信状态…'));
 try{const data=await api('/api/diagnostics',{},30000);$('diagnostics').replaceChildren();for(const check of data.checks||[]){const row=node('div','diagnostic '+check.state);row.append(node('b','',check.label+' · '+({ok:'正常',warn:'待处理',manual:'需手动验证'}[check.state]||'待验证')),node('p','',check.detail));$('diagnostics').append(row);}await refreshOverview();}catch(err){$('diagnostics').replaceChildren(node('p','error',err.message));}finally{btn.disabled=false;btn.textContent='检查连接问题';}
});
$('export-backup').addEventListener('click',async()=>{
 try{const data=await api('/api/export'),blob=new Blob([JSON.stringify(data,null,2)],{type:'application/json'}),url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;a.download='模块短信备份-'+new Date().toISOString().slice(0,10)+'.json';document.body.append(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(url),10000);text('backup-result','已生成 '+data.messages.length+' 条短信的备份。请在下载列表或“文件”中保存。');}catch(e){text('backup-result',e.message);}
});
$('import-backup').addEventListener('click',()=>$('backup-file').click());
$('backup-file').addEventListener('change',async()=>{
 const file=$('backup-file').files[0];if(!file)return;
 try{if(file.size>1048576)throw new Error('备份文件不能超过 1 MB');const data=JSON.parse(await file.text());if(data.version!==1||!Array.isArray(data.messages))throw new Error('这不是支持的短信备份文件');if(!await ask('恢复短信备份？','将合并 '+data.messages.length+' 条记录，保留原有短信，不会重新发送。','合并恢复'))return;const result=await api('/api/restore',data);text('backup-result',result.note);await refreshMessages();}catch(e){text('backup-result',e.message);}finally{$('backup-file').value='';}
});
$('rollback-version').addEventListener('click',async()=>{
 if(!await ask('恢复上一个网页版本？','网页会暂时断开，短信和访问密码会保留。确认没有正在发送的短信后继续。','恢复版本'))return;
 try{await api('/api/maintenance/rollback',{});toast('正在恢复上一个版本，请等待约 15 秒后刷新页面');connection(false);text('connection-text','正在切换网页版本，请稍后刷新');}catch(e){toast(e.message);}
});
function resizeViewport(){const v=window.visualViewport,root=document.documentElement;root.style.setProperty('--app-height',(v?v.height:window.innerHeight)+'px');root.style.setProperty('--app-top',(v?v.offsetTop:0)+'px');}
if(window.visualViewport){window.visualViewport.addEventListener('resize',resizeViewport);window.visualViewport.addEventListener('scroll',resizeViewport);}window.addEventListener('resize',resizeViewport);window.addEventListener('pagehide',saveDraft);
document.addEventListener('visibilitychange',()=>{if(!document.hidden){refreshMessages();checkPending();if(S.page==='network')refreshOverview();}});
pollTimer=setInterval(()=>{if(!document.hidden){refreshMessages();checkPending();}},4000);overviewTimer=setInterval(()=>{if(!document.hidden&&!$('main').hidden&&(S.page==='network'||S.page==='device'))refreshOverview();},5000);
resizeViewport();receiveBrowserLogin().then(boot);
})();
