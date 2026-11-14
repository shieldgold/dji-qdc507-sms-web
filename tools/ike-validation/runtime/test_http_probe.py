"""Computer-only tests; all HTTP fixtures bind loopback inside the test host."""
import os
import socket
import subprocess
import threading
import time
import unittest


class HTTPProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.probe = os.environ.get('QDC_HTTP_PROBE')
        if not cls.probe:
            raise unittest.SkipTest('set QDC_HTTP_PROBE to the standalone probe test binary')

    def request(self, response, *, delay=0, chunks=False):
        server = socket.socket()
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(('127.0.0.1', 8787))
        server.listen(1)
        received = []
        failures = []

        def serve():
            try:
                server.settimeout(4)
                with server.accept()[0] as client:
                    client.settimeout(4)
                    request = b''
                    while b'\r\n\r\n' not in request:
                        part = client.recv(4096)
                        if not part:
                            break
                        request += part
                    received.append(request)
                    if delay:
                        time.sleep(delay)
                    if chunks:
                        for byte in response:
                            client.sendall(bytes([byte]))
                    else:
                        client.sendall(response)
            except (BrokenPipeError, ConnectionResetError):
                pass  # The probe is expected to reject several response fixtures.
            except Exception as error:
                failures.append(error)
            finally:
                server.close()

        worker = threading.Thread(target=serve, daemon=True)
        worker.start()
        started = time.monotonic()
        result = subprocess.run([self.probe, '127.0.0.1'], capture_output=True, timeout=4)
        elapsed = time.monotonic() - started
        worker.join(4)
        self.assertFalse(worker.is_alive())
        self.assertFalse(failures)
        self.assertEqual(result.stderr, b'')
        self.assertEqual(len(result.stdout.splitlines()), 1)
        self.assertRegex(result.stdout, rb'^HTTP_PROBE status=\d+ transport=[01] bytes=\d+\n$')
        self.assertEqual(received, [b'GET / HTTP/1.0\r\nHost: 127.0.0.1:8787\r\nConnection: close\r\n\r\n'])
        return result, elapsed

    def test_content_length_success_never_outputs_body(self):
        secret = b'sms-body-must-not-appear'
        result, _ = self.request(b'HTTP/1.1 200 OK\r\nContent-Length: ' + str(len(secret)).encode() +
                                 b'\r\nX-Secret: hidden-header\r\n\r\n' + secret)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b'HTTP_PROBE status=200 transport=1 bytes=24\n')

    def test_http10_close_delimited_success(self):
        result, _ = self.request(b'HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nok', chunks=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn(b'bytes=2', result.stdout)

    def test_body_is_discarded_as_bytes_without_interpretation(self):
        result, _ = self.request(b'HTTP/1.0 200 OK\r\nContent-Length: 4\r\n\r\n\x00a\x00b')
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b'HTTP_PROBE status=200 transport=1 bytes=4\n')

    def test_non200_rejected_without_body(self):
        result, _ = self.request(b'HTTP/1.1 503 Unavailable\r\nContent-Length: 6\r\n\r\nsecret')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'status=503 transport=1', result.stdout)

    def test_malformed_status_and_headers_rejected(self):
        for response in (b'NOTHTTP 200 OK\r\n\r\n', b'HTTP/2.0 200 OK\r\n\r\n',
                         b'HTTP/1.1 2000 OK\r\n\r\n', b'HTTP/1.1 200 OK\r\nBroken\r\n\r\n',
                         b'HTTP/1.1 200 OK\r\nX: a\x00b\r\n\r\n'):
            with self.subTest(response=response):
                result, _ = self.request(response)
                self.assertNotEqual(result.returncode, 0)

    def test_content_length_mismatch_rejected(self):
        for response in (b'HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nab',
                         b'HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nab'):
            with self.subTest(response=response):
                result, _ = self.request(response)
                self.assertNotEqual(result.returncode, 0)

    def test_ambiguous_or_unsupported_framing_rejected(self):
        for headers in (b'Content-Length: 1\r\nContent-Length: 1',
                        b'Content-Length: -1', b'Content-Length: 1x',
                        b'Transfer-Encoding: chunked', b'Content-Length: 99999999999999999999999'):
            with self.subTest(headers=headers):
                result, _ = self.request(b'HTTP/1.1 200 OK\r\n' + headers + b'\r\n\r\nx')
                self.assertNotEqual(result.returncode, 0)

    def test_response_size_limits(self):
        for response in (b'HTTP/1.1 200 OK\r\nX: ' + b'h' * 8192 + b'\r\n\r\n',
                         b'HTTP/1.1 200 OK\r\n\r\n' + b'b' * (1024*1024+1),
                         b'HTTP/1.1 200 OK\r\nContent-Length: 1048577\r\n\r\n'):
            with self.subTest(size=len(response)):
                result, _ = self.request(response)
                self.assertNotEqual(result.returncode, 0)

    def test_absolute_deadline(self):
        result, elapsed = self.request(b'HTTP/1.0 200 OK\r\n\r\n', delay=2.5)
        self.assertNotEqual(result.returncode, 0)
        self.assertGreater(elapsed, 1.7)
        self.assertLess(elapsed, 2.4)
        self.assertIn(b'transport=1', result.stdout)

    def test_refused_port_reports_transport_failure(self):
        result = subprocess.run([self.probe, '127.0.0.1'], capture_output=True, timeout=3)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b'HTTP_PROBE status=0 transport=0 bytes=0\n')

    def test_public_nonlocal_and_nonliteral_addresses_rejected(self):
        for address in ('8.8.8.8', '192.168.251.239', '127.0.0.2', '0.0.0.0',
                        '169.254.1.1', 'localhost', '::1', '127.0.0.1:8787'):
            with self.subTest(address=address):
                result = subprocess.run([self.probe, address], capture_output=True, timeout=1)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b'HTTP_PROBE status=0 transport=0 bytes=0\n')


if __name__ == '__main__':
    unittest.main()
