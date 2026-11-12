const test=require('node:test');const assert=require('node:assert/strict');
const SMS=require('../../web/model.js');
test('numbers keep domestic and international forms distinct',()=>{
 assert.equal(SMS.normalizeNumber(' +86 (138) 0013-8000 '),'+8613800138000');
 assert.equal(SMS.normalizeNumber('10691859839103'),'10691859839103');
 assert.equal(SMS.validNumber('1;ATD'),false);assert.equal(SMS.validNumber('+'),false);
});
test('SMS counters handle GSM extensions and UTF16 boundaries',()=>{
 assert.deepEqual(SMS.segments('a'.repeat(160)),{count:1,units:160,encoding:'GSM-7'});
 assert.equal(SMS.segments('^'.repeat(81)).count,2);
 assert.equal(SMS.segments('中'.repeat(70)).count,1);
 assert.equal(SMS.segments('中'.repeat(71)).count,2);
 assert.equal(SMS.segments('😀'.repeat(34)).count,1);
 assert.equal(SMS.segments('😀'.repeat(36)).count,2);
 assert.equal(SMS.segments('').count,0);
});
test('threads group replies and search full message history',()=>{
 const ms=[{key:'a',sender:'10010',body:'old search',time:'2026-11-12 10:00:00',direction:'in'},{key:'b',sender:'10010',body:'reply',time:'2026-11-12T11:00:00Z',direction:'out'},{key:'c',sender:'10086',body:'other',time:'2026-11-12T09:00:00Z',direction:'in'}];
 let list=SMS.threads(ms,'old search',{},{});assert.equal(list.length,1);assert.equal(list[0].messages.length,2);assert.equal(list[0].latest.key,'b');
 list=SMS.threads(ms,'',{},{});assert.equal(list[0].number,'10010');assert.equal(list[0].unread,1);
 assert.equal(SMS.threads(ms,'',{'a':true},{})[0].unread,0);
});
