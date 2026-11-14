"""Only loopback test datagrams; no real VPN or USB access."""
import importlib.util
from pathlib import Path
import socket
import unittest

HERE = Path(__file__).resolve().parent


class RelayTests(unittest.TestCase):
    def load(self):
        self.assertTrue((HERE/'udp_relay.py').exists(), 'bounded UDP relay is not implemented')
        spec = importlib.util.spec_from_file_location('udp_relay', HERE/'udp_relay.py')
        m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
        return m

    def ike(self, exchange):
        header = b'12345678'+b'ABCDEFGH'+bytes([0,32,exchange,8])+b'\x00'*4+(28).to_bytes(4,'big')
        return b'\x00'*4+header

    def test_init_marker_translation_and_natt_passthrough(self):
        m = self.load()
        packet = self.ike(34)
        port, data = m.to_server(packet)
        self.assertEqual(port, 500); self.assertEqual(data, packet[4:])
        self.assertEqual(m.to_client(500, data), packet)
        for packet in [self.ike(35), b'ESP1'+b'\x00'*32, b'\xff']:
            self.assertEqual(m.to_server(packet), (4500, packet))
            self.assertEqual(m.to_client(4500, packet), packet)

    def test_malformed_or_oversized_messages_rejected(self):
        m = self.load()
        for packet in [b'', b'\x00'*7, b'\x00'*32, b'x'*9000]:
            with self.subTest(size=len(packet)), self.assertRaises(ValueError): m.to_server(packet)
        with self.assertRaises(ValueError): m.to_client(53, self.ike(34))
        with self.assertRaises(ValueError): m.to_client(4500, b'x'*9000)

    def test_real_udp_roundtrip_and_connected_peer_filter(self):
        m = self.load()
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client, \
             socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as ike, \
             socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as esp, \
             socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as stranger:
            for sock in (client, ike, esp, stranger):
                sock.bind(('127.0.0.1', 0)); sock.settimeout(.5)
            relay = m.Relay(('127.0.0.1', 0), client.getsockname(), '127.0.0.1',
                            {500: ike.getsockname()[1], 4500: esp.getsockname()[1]})
            address = relay.front.getsockname()
            try:
                for packet, backend, expected in [(self.ike(34), ike, self.ike(34)[4:]),
                                                  (b'ESP1'+b'X'*128, esp, b'ESP1'+b'X'*128)]:
                    client.sendto(packet, address); relay.pump(.1)
                    received, source = backend.recvfrom(9000)
                    self.assertEqual(received, expected)
                    backend.sendto(received, source); relay.pump(.1)
                    self.assertEqual(client.recv(9000), packet)
                stranger.sendto(self.ike(34), address); relay.pump(.05)
                self.assertEqual(relay.counts['client_packets'], 2)
                client.sendto(b'X'*9000, address); relay.pump(.1)
                self.assertEqual(relay.counts['rejected'], 1)
            finally:
                relay.close()
            self.assertEqual(relay.front.fileno(), -1)


if __name__ == '__main__':
    unittest.main()
