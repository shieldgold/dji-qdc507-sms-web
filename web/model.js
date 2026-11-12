(function(root){
'use strict';
const basic=new Set(Array.from('@£$¥èéùìòÇ\nØø\rÅåΔ_ΦΓΛΩΠΨΣΘΞÆæßÉ !"#¤%&\'()*+,-./0123456789:;<=>?¡ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÑÜ§¿abcdefghijklmnopqrstuvwxyzäöñüà'));
const extended=new Set(Array.from('\f^{}\\[~]|€'));
function normalizeNumber(value){return String(value||'').trim().replace(/[\s()\-]/g,'');}
function validNumber(value){return /^\+?[0-9]{1,20}$/.test(normalizeNumber(value));}
function segments(text){
 const chars=Array.from(text||'');if(!chars.length)return {count:0,units:0,encoding:'GSM-7'};
 const gsm=chars.every(c=>basic.has(c)||extended.has(c));
 const widths=chars.map(c=>gsm?(extended.has(c)?2:1):c.length);
 const units=widths.reduce((a,b)=>a+b,0);const single=gsm?160:70,multipart=gsm?153:67;
 let count=1;if(units>single){let used=0;count=1;for(const w of widths){if(used+w>multipart){count++;used=0;}used+=w;}}
 return {count,units,encoding:gsm?'GSM-7':'Unicode'};
}
function timestamp(value){
 if(!value)return 0;
 // Legacy module archives have no zone. Preserve displayed local timestamps.
 const t=Date.parse(String(value).replace(' ','T'));return Number.isFinite(t)?t:0;
}
function threads(messages,query,seen,favorites){
 const groups=new Map();const term=String(query||'').trim().toLowerCase();
 for(const message of messages||[]){const number=normalizeNumber(message.sender)||String(message.sender||'未知号码');
  if(!groups.has(number))groups.set(number,{number,messages:[],unread:0,favorite:false});
  const t=groups.get(number);t.messages.push(message);if(message.direction!=='out'&&!seen[message.key])t.unread++;if(favorites[message.key])t.favorite=true;
 }
 return Array.from(groups.values()).map(t=>{t.messages.sort((a,b)=>timestamp(a.time)-timestamp(b.time));t.latest=t.messages[t.messages.length-1];return t;})
 .filter(t=>!term||t.number.toLowerCase().includes(term)||t.messages.some(m=>String(m.body||'').toLowerCase().includes(term)))
 .sort((a,b)=>timestamp(b.latest.time)-timestamp(a.latest.time));
}
const api={normalizeNumber,validNumber,segments,timestamp,threads};
if(typeof module!=='undefined'&&module.exports)module.exports=api;else root.SMS=api;
})(typeof window!=='undefined'?window:globalThis);
