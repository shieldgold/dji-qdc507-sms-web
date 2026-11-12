package main

// ui.go — 内嵌网页界面（单页，三态：首次注册密码 / 登录 / 短信主界面）
//
// 密码按 SIM 卡（ICCID）分别注册，换卡就是另一套密码。

const indexHTML = `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-title" content="模块短信">
<title>模块短信</title>
<style>
:root{--bg:#f2f2f7;--card:#fff;--fg:#1c1c1e;--sub:#8e8e93;--accent:#007aff;--line:#e5e5ea;--danger:#ff3b30}
@media (prefers-color-scheme:dark){:root{--bg:#000;--card:#1c1c1e;--fg:#fff;--sub:#8e8e93;--line:#2c2c2e}}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 -apple-system,BlinkMacSystemFont,"PingFang SC",sans-serif}
header{position:sticky;top:0;z-index:9;background:var(--bg);border-bottom:1px solid var(--line);padding:10px 16px;padding-top:calc(10px + env(safe-area-inset-top))}
h1{font-size:17px;margin:0 0 3px;display:flex;justify-content:space-between;align-items:center}
.st{font-size:12px;color:var(--sub);display:flex;gap:12px;flex-wrap:wrap}
.dot{display:inline-block;width:7px;height:7px;border-radius:50%;background:#34c759;margin-right:4px;vertical-align:1px}
.dot.bad{background:#ff3b30}
.wrap{padding:14px 16px 130px;max-width:760px;margin:0 auto}
.card{background:var(--card);border-radius:14px;padding:14px;margin-bottom:10px}
.row{display:flex;justify-content:space-between;align-items:baseline;gap:8px}
.who{font-weight:600;font-size:15px}
.when{font-size:12px;color:var(--sub);white-space:nowrap}
.body{margin-top:6px;white-space:pre-wrap;word-break:break-word;font-size:15px}
.tag{display:inline-block;font-size:11px;color:var(--sub);border:1px solid var(--line);border-radius:6px;padding:0 5px;margin-right:6px}
.code{margin-top:10px;display:flex;align-items:center;gap:12px}
.code b{font-size:26px;letter-spacing:2px;font-family:ui-monospace,Menlo,monospace}
button{font:inherit;border:0;border-radius:10px;padding:9px 14px;background:var(--accent);color:#fff;font-weight:600}
button:disabled{opacity:.5}
button.ghost{background:transparent;color:var(--accent);font-weight:500;padding:6px 8px;font-size:14px}
button.danger{background:transparent;color:var(--danger);font-weight:500;padding:6px 8px}
button.wide{width:100%;padding:13px}
footer{position:fixed;left:0;right:0;bottom:0;background:var(--bg);border-top:1px solid var(--line);padding:10px 16px;padding-bottom:calc(10px + env(safe-area-inset-bottom));display:flex;gap:8px}
input,textarea{font:inherit;border:1px solid var(--line);border-radius:10px;padding:11px;background:var(--card);color:var(--fg);width:100%}
textarea{resize:none;height:42px}
.empty{text-align:center;color:var(--sub);padding:50px 0}
.toast{position:fixed;left:50%;transform:translateX(-50%);bottom:110px;background:#000c;color:#fff;padding:9px 16px;border-radius:20px;font-size:14px;opacity:0;transition:.25s;pointer-events:none;max-width:80%;text-align:center}
.toast.on{opacity:1}
.gate{max-width:420px;margin:0 auto;padding:34px 20px}
.gate .logo{font-size:40px;text-align:center;margin-bottom:10px}
.gate h2{text-align:center;font-size:20px;margin:0 0 6px}
.gate p{text-align:center;color:var(--sub);font-size:13px;margin:0 0 20px;line-height:1.6}
.field{margin-bottom:12px}
.check{display:flex;align-items:center;gap:9px;margin:14px 0 18px;font-size:14px;color:var(--fg)}
.check input{width:20px;height:20px;flex:0 0 20px}
.hint{font-size:12px;color:var(--sub);margin-top:10px;text-align:center}
.sim{background:var(--card);border-radius:12px;padding:12px;margin-bottom:18px;font-size:13px}
.sim div{display:flex;justify-content:space-between;padding:3px 0}
.sim span:last-child{color:var(--sub);font-family:ui-monospace,Menlo,monospace;font-size:12px}
.err{background:rgba(255,59,48,.12);color:var(--danger);border-radius:10px;padding:9px 12px;font-size:13px;margin-bottom:12px;display:none}
.setrow{display:flex;gap:8px;justify-content:flex-end;margin-top:8px}
</style>
</head>
<body>

<!-- ============ 首次注册 ============ -->
<div id="setup" class="gate" style="display:none">
  <div class="logo">🔐</div>
  <h2>设置访问密码</h2>
  <p>这是该号码第一次使用，请为它设置一个访问密码。<br>密码只保存在模块里，用于保护短信内容。</p>
  <div class="sim" id="sim-setup"></div>
  <div class="err" id="err-setup"></div>
  <div class="field"><input id="pw1" type="password" placeholder="设置密码（至少 4 位）" autocomplete="new-password"></div>
  <div class="field"><input id="pw2" type="password" placeholder="再输入一次" autocomplete="new-password"></div>
  <label class="check"><input id="rm-setup" type="checkbox" checked>在本机记住登录（下次免输入）</label>
  <button class="wide" id="btn-setup">设置密码并进入</button>
  <div class="hint">忘记密码时，可在电脑上用 ADB 删除<br><code>/data/smsweb/auth.json</code> 后重新设置</div>
</div>

<!-- ============ 登录 ============ -->
<div id="login" class="gate" style="display:none">
  <div class="logo">🔒</div>
  <h2>请输入访问密码</h2>
  <p>该号码已设置过密码</p>
  <div class="sim" id="sim-login"></div>
  <div class="err" id="err-login"></div>
  <div class="field"><input id="pw" type="password" placeholder="访问密码" autocomplete="current-password"></div>
  <label class="check"><input id="rm-login" type="checkbox" checked>在本机记住登录（下次免输入）</label>
  <button class="wide" id="btn-login">登录</button>
</div>

<!-- ============ SIM PIN ============ -->
<div id="pin" class="gate" style="display:none">
  <div class="logo">🔓</div><h2>解锁 SIM 卡</h2>
  <p>模块断电后需要重新输入 SIM PIN。<br>PIN 只用于本次解锁，不会保存到模块。</p>
  <div class="sim"><span id="pin-state">正在检查 SIM…</span></div>
  <div class="err" id="err-pin" role="alert"></div>
  <div class="field"><input id="pin-value" type="password" inputmode="numeric" maxlength="8" placeholder="SIM PIN（4–8 位数字）" autocomplete="off"></div>
  <button class="wide" id="btn-pin-unlock">解锁一次</button>
  <button class="wide danger" id="btn-pin-retry" style="display:none;margin-top:14px">已核实正确 PIN，允许再试一次</button>
  <div style="text-align:center;margin-top:12px"><button class="ghost" id="btn-pin-refresh">刷新 SIM 状态</button></div>
  <div class="hint">PIN 错误可能消耗 SIM 重试次数。程序不会自动重试。</div>
</div>

<!-- ============ 主界面 ============ -->
<div id="main" style="display:none">
  <header>
    <h1>模块短信 <span><button class="ghost" id="btn-chpw">改密码</button><button class="ghost" id="btn-logout">退出</button></span></h1>
    <div class="st">
      <span><span class="dot" id="dot"></span><span id="carrier">连接中…</span></span>
      <span id="signal"></span>
      <span id="storage"></span>
      <span id="updated"></span>
    </div>
  </header>
  <div class="wrap" id="list"><div class="empty">加载中…</div></div>
  <footer>
    <input id="num" placeholder="号码" inputmode="tel" style="flex:0 0 33%">
    <textarea id="txt" placeholder="短信内容"></textarea>
    <button id="send">发送</button>
  </footer>
</div>

<!-- ============ 修改密码 ============ -->
<div id="chpw" class="gate" style="display:none">
  <div class="logo">🔑</div>
  <h2>修改访问密码</h2>
  <div class="err" id="err-chpw"></div>
  <div class="field"><input id="oldpw" type="password" placeholder="当前密码" autocomplete="current-password"></div>
  <div class="field"><input id="newpw1" type="password" placeholder="新密码（至少 4 位）" autocomplete="new-password"></div>
  <div class="field"><input id="newpw2" type="password" placeholder="再输入一次新密码" autocomplete="new-password"></div>
  <button class="wide" id="btn-chpw-save">保存</button>
  <div style="text-align:center;margin-top:12px"><button class="ghost" id="btn-chpw-back">返回</button></div>
</div>

<div class="toast" id="toast"></div>
<script>
var $=function(id){return document.getElementById(id)};
function toast(t){var e=$('toast');e.textContent=t;e.classList.add('on');setTimeout(function(){e.classList.remove('on')},2200)}
function esc(s){return String(s==null?'':s).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]})}
function showErr(id,msg){var e=$(id);if(!msg){e.style.display='none';return}e.textContent=msg;e.style.display='block'}
function view(name){
  ['setup','login','main','chpw','pin'].forEach(function(v){$(v).style.display=(v===name?'':'none')});
}
var SIM=null;
function simBox(el,st){
  var num=st.number||'（该卡未写入本机号码）';
  var iccid=st.iccid||'未知';
  $(el).innerHTML='<div><span>本机号码</span><span>'+esc(num)+'</span></div>'+
                  '<div><span>卡号 ICCID</span><span>'+esc(iccid)+'</span></div>';
}
function cookieGet(n){var m=document.cookie.match(new RegExp('(^| )'+n+'=([^;]+)'));return m?m[2]:''}

function boot(){
  fetch('api/auth/status',{cache:'no-store'}).then(function(r){return r.json()}).then(function(st){
    SIM=st;
    if(!st.registered){simBox('sim-setup',st);view('setup');$('pw1').focus();return}
    if(!st.authenticated){simBox('sim-login',st);view('login');$('pw').focus();return}
    if(st.sim&&st.sim.state!=='READY'){showPIN(st.sim);return}
    view('main');load();
  }).catch(function(){view('login');showErr('err-login','无法连接模块，请稍后重试')})
}

function post(url,body){
  return fetch(url,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(function(r){return r.json().then(function(d){d._status=r.status;return d})})
}

var pinSending=false;
function showPIN(st){
  view('pin');
  var label={'SIM PIN':'SIM 已锁定，请输入 PIN','SIM PUK':'SIM 需要 PUK，请在手机的 SIM 管理中处理','READY':'SIM 已就绪'};
  $('pin-state').textContent=label[st.state]||('SIM 状态：'+(st.state||'未知'));
  $('btn-pin-unlock').disabled=pinSending||st.blocked||st.state!=='SIM PIN';
  $('pin-value').disabled=pinSending||st.blocked||st.state!=='SIM PIN';
  $('btn-pin-retry').style.display=st.blocked&&st.state==='SIM PIN'?'':'none';
}
$('btn-pin-refresh').onclick=function(){showErr('err-pin','');boot()};
$('btn-pin-unlock').onclick=function(){
  if(pinSending)return;
  var value=$('pin-value').value;
  if(!/^[0-9]{4,8}$/.test(value)){showErr('err-pin','PIN 必须为 4–8 位数字');return}
  pinSending=true;this.disabled=true;this.textContent='正在解锁…';showErr('err-pin','');
  post('api/sim/unlock',{pin:value,request_id:Date.now().toString(36)+'-'+Math.random().toString(36).slice(2)}).then(function(d){
    $('pin-value').value='';pinSending=false;$('btn-pin-unlock').textContent='解锁一次';
    if(d._status===401){boot();return}
    if(d.error){if(d.sim)showPIN(d.sim);showErr('err-pin',d.error);return}
    boot();
  }).catch(function(){
    $('pin-value').value='';pinSending=false;$('btn-pin-unlock').textContent='解锁一次';
    showErr('err-pin','请求中断，结果不确定。请先刷新 SIM 状态，不要直接重试。');
  });
};
$('btn-pin-retry').onclick=function(){
  if(!confirm('请确认已核实正确 PIN。再次输入错误可能锁卡，是否允许再试一次？'))return;
  this.disabled=true;
  post('api/sim/retry',{confirmed:true}).then(function(d){
    $('btn-pin-retry').disabled=false;
    if(d.error){showErr('err-pin',d.error);return}
    showErr('err-pin','');showPIN(d);
  }).catch(function(){$('btn-pin-retry').disabled=false;showErr('err-pin','连接中断，请刷新状态')});
};

$('btn-setup').onclick=function(){
  var p1=$('pw1').value,p2=$('pw2').value;
  showErr('err-setup','');
  if(p1.length<4){showErr('err-setup','密码至少 4 位');return}
  if(p1!==p2){showErr('err-setup','两次输入的密码不一致');return}
  this.disabled=true;this.textContent='设置中…';
  var btn=this;
  post('api/auth/setup',{password:p1,confirm:p2,remember:$('rm-setup').checked}).then(function(d){
    btn.disabled=false;btn.textContent='设置密码并进入';
    if(d.error){showErr('err-setup',d.error);return}
    $('pw1').value=$('pw2').value='';
    toast('密码已设置');
    boot();
  }).catch(function(){btn.disabled=false;btn.textContent='设置密码并进入';showErr('err-setup','网络错误')})
};

$('btn-login').onclick=function(){
  var p=$('pw').value;showErr('err-login','');
  if(!p){showErr('err-login','请输入密码');return}
  var btn=this;btn.disabled=true;btn.textContent='登录中…';
  post('api/auth/login',{password:p,remember:$('rm-login').checked}).then(function(d){
    btn.disabled=false;btn.textContent='登录';
    if(d.error){showErr('err-login',d.error);return}
    $('pw').value='';
    boot();
  }).catch(function(){btn.disabled=false;btn.textContent='登录';showErr('err-login','网络错误')})
};
$('pw').addEventListener('keydown',function(e){if(e.key==='Enter')$('btn-login').click()});

$('btn-logout').onclick=function(){
  post('api/auth/logout',{}).then(function(){view('login');simBox('sim-login',SIM||{});$('pw').focus()})
};
$('btn-chpw').onclick=function(){showErr('err-chpw','');view('chpw')};
$('btn-chpw-back').onclick=function(){view('main')};
$('btn-chpw-save').onclick=function(){
  var o=$('oldpw').value,n1=$('newpw1').value,n2=$('newpw2').value;
  showErr('err-chpw','');
  if(n1.length<4){showErr('err-chpw','新密码至少 4 位');return}
  if(n1!==n2){showErr('err-chpw','两次输入的新密码不一致');return}
  var btn=this;btn.disabled=true;
  post('api/auth/change',{old:o,password:n1,confirm:n2,remember:$('rm-login')?$('rm-login').checked:true}).then(function(d){
    btn.disabled=false;
    if(d.error){showErr('err-chpw',d.error);return}
    $('oldpw').value=$('newpw1').value=$('newpw2').value='';
    toast('密码已修改');view('main');
  }).catch(function(){btn.disabled=false;showErr('err-chpw','网络错误')})
};

/* ---------- 主界面 ---------- */
function copycode(c){
  if(navigator.clipboard&&navigator.clipboard.writeText){navigator.clipboard.writeText(c).then(function(){toast('验证码已复制')},function(){toast(c)})}
  else{var t=document.createElement('textarea');t.value=c;document.body.appendChild(t);t.select();try{document.execCommand('copy');toast('验证码已复制')}catch(e){toast(c)}document.body.removeChild(t)}
}
function reply(n){$('num').value=n;$('txt').focus();window.scrollTo(0,document.body.scrollHeight)}
function del(k){
  if(!confirm('删除这条短信？'))return;
  post('api/delete',{key:k}).then(function(d){
    if(d._status===401){view('login');return}
    toast(d.note||'已删除');load();
  }).catch(function(){toast('删除失败')})
}
function load(){
  fetch('api/state',{cache:'no-store'}).then(function(r){
    if(r.status===401){view('login');simBox('sim-login',SIM||{});$('pw').focus();return null}
    return r.json();
  }).then(function(d){
    if(!d)return;
    if(d.sim&&d.sim.state!=='READY'){showPIN(d.sim);return}
    var st=d.status||{};
    var good=st.simReady&&st.signal&&st.signal!=='无信号';
    $('dot').className='dot'+(good?'':' bad');
    $('carrier').textContent=st.carrier||st.identity||'未知运营商';
    $('signal').textContent=st.signal?('信号 '+st.signal):'';
    $('storage').textContent=st.storage?('存储 '+st.storage):'';
    $('updated').textContent=(d.lastPoll?('更新 '+d.lastPoll):'')+(d.lastError?' ⚠︎':'');
    var ms=d.messages||[],L=$('list'),h='';
    if(!ms.length){L.innerHTML='<div class="empty">暂无短信</div>';return}
    for(var i=0;i<ms.length;i++){
      var m=ms[i];
      var code=m.code?'<div class="code"><b>'+esc(m.code)+'</b><button class="ghost" onclick="copycode(\''+esc(m.code)+'\')">复制</button></div>':'';
      h+='<div class="card"><div class="row"><div class="who">'+esc(m.sender)+'</div><div class="when">'+esc(m.time)+'</div></div>'+
         '<div><span class="tag">'+(m.direction==='out'?'发出':'收到')+'</span><span class="tag">'+esc(m.status||'')+'</span></div>'+
         '<div class="body">'+esc(m.body)+'</div>'+code+
         '<div style="margin-top:8px"><button class="ghost" onclick="reply(\''+esc(m.sender)+'\')">回复</button>'+
         '<button class="danger" onclick="del(\''+esc(m.key)+'\')">删除</button></div></div>';
    }
    L.innerHTML=h;
  }).catch(function(){})
}
$('send').onclick=function(){
  var n=$('num').value.trim(),t=$('txt').value;
  if(!n||!t.trim()){toast('请填写号码和内容');return}
  var btn=this;btn.disabled=true;btn.textContent='发送中';
  post('api/send',{number:n,text:t}).then(function(d){
    btn.disabled=false;btn.textContent='发送';
    if(d._status===401){view('login');return}
    if(d.error){toast(d.error)}else{$('txt').value='';toast('已提交到网络，送达尚未确认');load()}
  }).catch(function(){btn.disabled=false;btn.textContent='发送';toast('连接中断，发送结果不确定，请核对接收端后再决定是否重发')})
};
setInterval(function(){if($('main').style.display!=='none')load()},4000);
boot();
</script>
</body>
</html>`
