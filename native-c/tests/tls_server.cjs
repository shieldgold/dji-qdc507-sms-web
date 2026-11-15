'use strict';
const fs = require('node:fs');
const http2 = require('node:http2');
const https = require('node:https');
const tls = require('node:tls');
const mode = process.argv[4];
const options = {key: fs.readFileSync(process.argv[2]), cert: fs.readFileSync(process.argv[3]),
  minVersion: mode === 'tls12' ? 'TLSv1.2' : 'TLSv1.3',
  maxVersion: mode === 'tls12' ? 'TLSv1.2' : 'TLSv1.3'};
let server;
if (mode.startsWith('bad-h1')) {
  server = tls.createServer({...options, ALPNProtocols: ['http/1.1']}, socket => {
    socket.on('error', () => {});
    socket.once('data', () => socket.end(mode === 'bad-h1-lf' ?
      'HTTP/1.1 200 OK\r\nX-Test: a\nb\r\nDate: Sun, 15 Nov 2026 00:00:00 GMT\r\n\r\n' :
      'HTTP/1.1 200 OK\r\nDate: Sun, 15 Nov 2026 00:00:00 GMT\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n'));
  });
} else if (mode === 'h1') {
  server = https.createServer(options, (request, response) => {
    response.writeHead(200, {'Date': 'Sun, 15 Nov 2026 00:00:00 GMT'}); response.end('clock');
  });
} else {
  server = http2.createSecureServer({...options, allowHTTP1: false});
  server.on('stream', (stream, headers) => {
    stream.on('error', () => {});
    let body = '';
    stream.on('data', data => body += data);
    stream.on('end', () => {
      if (headers.authorization !== 'bearer synthetic-probe' || body !== '{}') {
        stream.respond({':status': 400}); stream.end(); return;
      }
      if (mode === 'delay') { setTimeout(() => { if (!stream.destroyed) { stream.respond({':status': 200}); stream.end(); } }, 2000); return; }
      stream.respond({':status': mode === 'redirect' ? 302 : mode === 'failure' ? 410 : 200,
        'date': 'Sun, 15 Nov 2026 00:00:00 GMT', ...(mode === 'redirect' ? {location: 'https://example.invalid/never-follow'} : {})});
      if (mode === 'reset') { stream.write('partial'); setTimeout(() => stream.destroy(new Error('synthetic reset')), 20); }
      else if (mode === 'oversize') stream.end('x'.repeat(9000));
      else stream.end(mode === 'failure' ? '{"reason":"Unregistered"}' : '{}');
    });
  });
}
server.on('tlsClientError', () => {});
server.on('sessionError', () => {});
server.listen(0, '127.0.0.1', () => process.stdout.write(JSON.stringify({port: server.address().port}) + '\n'));
