"""Bounded UDP transport for one QDC507 client and one IKE endpoint.

No payloads, credentials, routing changes or firewall changes are recorded.
"""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import selectors
import signal
import socket
import time

MAX_PACKET = 8192
MARKER = b'\0' * 4


def ike_exchange(packet):
    if len(packet) < 28 or packet[17] != 0x20 or int.from_bytes(packet[24:28], 'big') != len(packet):
        raise ValueError('invalid IKEv2 length/version')
    return packet[18]


def natt(packet):
    if not packet or len(packet) > MAX_PACKET:
        raise ValueError('UDP length outside bound')
    if packet == b'\xff':
        return
    if packet.startswith(MARKER):
        ike_exchange(packet[4:])
    elif len(packet) < 8:
        raise ValueError('truncated ESP')


def to_server(packet):
    natt(packet)
    if packet.startswith(MARKER) and ike_exchange(packet[4:]) == 34:
        return 500, packet[4:]
    return 4500, packet


def to_client(port, packet):
    if port == 500:
        if len(packet) > MAX_PACKET - 4 or ike_exchange(packet) != 34:
            raise ValueError('unexpected port 500 response')
        return MARKER + packet
    if port != 4500:
        raise ValueError('server port outside allowlist')
    natt(packet)
    return packet


class Relay:
    def __init__(self, local, module, server, server_ports=None, journal=None):
        self.selector = selectors.DefaultSelector()
        self.sockets = []
        self.counts = {'client_packets': 0, 'server_packets': 0, 'rejected': 0, 'bootstrap_errors': 0}
        self.journal = journal or (lambda event: None)
        self.front = self.make(local, module, 'client')
        self.back = {}
        for port in (500, 4500):
            endpoint = (server, (server_ports or {}).get(port, port))
            self.back[port] = self.make(('', 0), endpoint, port)
        self.seen_client = False

    def make(self, local, remote, label):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.bind(local)
            sock.connect(remote)
            sock.setblocking(False)
            self.selector.register(sock, selectors.EVENT_READ, label)
            self.sockets.append(sock)
            return sock
        except BaseException:
            sock.close()
            self.close()
            raise

    def warmup(self):
        if not self.seen_client:
            try:
                self.front.send(b'\xff')
            except (ConnectionRefusedError, OSError):
                self.counts['bootstrap_errors'] += 1

    def pump(self, timeout=.25):
        for key, _ in self.selector.select(timeout):
            try:
                packet = key.fileobj.recv(MAX_PACKET + 1)
                if key.data == 'client':
                    port, out = to_server(packet)
                    self.seen_client = True
                    self.back[port].send(out)
                    self.counts['client_packets'] += 1
                else:
                    self.front.send(to_client(key.data, packet))
                    self.counts['server_packets'] += 1
            except ValueError:
                self.counts['rejected'] += 1
            except ConnectionRefusedError:
                if self.seen_client:
                    raise
                self.counts['bootstrap_errors'] += 1

    def close(self):
        for sock in self.sockets:
            sock.close()
        self.sockets.clear()
        self.selector.close()


def main():
    os.umask(0o077)
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('local', 'module', 'server'):
        p.add_argument('--'+name, required=True, type=ipaddress.IPv4Address)
    p.add_argument('--port', type=int, default=45000)
    p.add_argument('--seconds', type=int, default=180)
    p.add_argument('--journal', type=Path, required=True)
    a = p.parse_args()
    if not 1024 <= a.port <= 65535 or not 1 <= a.seconds <= 300:
        p.error('nonprivileged port and 1..300 seconds required')
    if any(ip.is_unspecified or ip.is_multicast or ip.is_loopback for ip in (a.local, a.module, a.server)):
        p.error('unicast physical endpoints required')
    stopping = False
    def stop(_sig, _frame):
        nonlocal stopping
        stopping = True
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, stop)
    with a.journal.open('x', encoding='utf-8') as log:
        def journal(event):
            log.write(json.dumps({'monotonic': time.monotonic(), **event})+'\n')
            log.flush()
            os.fsync(log.fileno())
        relay = Relay((str(a.local), a.port), (str(a.module), 4500), str(a.server), journal=journal)
        until = time.monotonic()+a.seconds
        next_warmup = 0
        next_status = 0
        journal({'event': 'ready', 'local': str(a.local), 'port': a.port,
                 'module': str(a.module), 'server': str(a.server), 'max_bytes': MAX_PACKET})
        print('UDP_RELAY_READY', flush=True)
        try:
            while not stopping and time.monotonic() < until:
                now = time.monotonic()
                if now >= next_warmup and until-now > a.seconds-15:
                    relay.warmup()
                    next_warmup = now+.5
                relay.pump()
                if now >= next_status:
                    journal({'event': 'counts', **relay.counts})
                    next_status = now+2
        finally:
            relay.close()
            journal({'event': 'closed', **relay.counts})


if __name__ == '__main__':
    main()
