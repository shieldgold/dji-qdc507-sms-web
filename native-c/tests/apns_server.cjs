'use strict';
const fs = require('node:fs');
const http2 = require('node:http2');
const crypto = require('node:crypto');
const mode = process.argv[5], kind = process.argv[6];
const publicKey = fs.readFileSync(process.argv[4]);
const server = http2.createSecureServer({key: fs.readFileSync(process.argv[2]), cert: fs.readFileSync(process.argv[3])});
server.on('tlsClientError', () => {}); server.on('sessionError', () => {});
server.on('stream', (stream, headers) => {
  stream.on('error', () => {}); let body = '';
  stream.on('data', bytes => { body += bytes; });
  stream.on('end', () => {
    if (headers[':method'] === 'GET' && headers[':path'] === '/library/test/success.html') {
      stream.respond({':status': 200, date: new Date().toUTCString()}); stream.end('{}'); return;
    }
    let valid = true;
    try {
      const token = headers.authorization.split(' ')[1], parts = token.split('.');
      const header = JSON.parse(Buffer.from(parts[0], 'base64url'));
      const claims = JSON.parse(Buffer.from(parts[1], 'base64url'));
      const signature = Buffer.from(parts[2], 'base64url');
      valid = parts.length === 3 && signature.length === 64 && header.alg === 'ES256' &&
        header.kid === 'ABC1234567' && claims.iss === 'TEAM123456' &&
        Math.abs(claims.iat - Date.now()/1000) < 60 &&
        crypto.verify('sha256', Buffer.from(parts[0] + '.' + parts[1]), {key: publicKey, dsaEncoding: 'ieee-p1363'}, signature);
      const payload = JSON.parse(body);
      valid &&= headers['apns-id'] === headers['apns-collapse-id'] && payload.event_id === headers['apns-id'] &&
        /^\/3\/device\/[ab]{64}$/.test(headers[':path']) && !body.includes('a'.repeat(64)) &&
        !body.includes('card-a') && !body.includes('13800000000');
      if (kind === 'call') {
        valid &&= headers['apns-topic'] === 'com.shield.modulephone.voip' && headers['apns-push-type'] === 'voip' &&
          headers['apns-expiration'] === '0' && Object.keys(payload.aps).length === 0 &&
          payload.call_id === 'c'.repeat(24) && typeof payload.call_uuid === 'string';
      } else {
        valid &&= headers['apns-topic'] === 'com.shield.modulephone' && headers['apns-push-type'] === 'alert' &&
          headers['apns-priority'] === '10' && Number(headers['apns-expiration']) > Date.now()/1000 &&
          typeof payload.aps.alert.title === 'string' && payload.aps.sound === 'default';
      }
    } catch { valid = false; }
    if (!valid) { stream.respond({':status': 400}); stream.end('{"reason":"OfflineFixtureMismatch"}'); return; }
    const status = mode === 'BadDeviceToken' ? 400 : mode === 'Unregistered' ? 410 :
      ['InvalidProviderToken', 'ExpiredProviderToken', 'TopicDisallowed'].includes(mode) ? 403 :
      mode === 'TooManyRequests' ? 429 : mode === 'ServiceUnavailable' ? 503 : 200;
    stream.respond({':status': status, date: new Date().toUTCString()}); stream.end(status === 200 ? '{}' : JSON.stringify({reason: mode}));
  });
});
server.listen(0, '127.0.0.1', () => process.stdout.write(JSON.stringify({port: server.address().port}) + '\n'));
