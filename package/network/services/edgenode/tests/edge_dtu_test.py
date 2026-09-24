"""通过真实 TCP 和 PTY 验证透传字节、广播、重连及客户端限额。"""
import os
import pty
import select
import socket
import subprocess
import sys
import time
import unittest
from concurrent.futures import ThreadPoolExecutor

FIXTURE = sys.argv.pop(1)

def listener():
    sock = socket.socket()
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('127.0.0.1', 0))
    sock.listen(8)
    sock.settimeout(5)
    return sock

def receive(sock, size, quick_ack=False):
    result = bytearray()
    while len(result) < size:
        if quick_ack and hasattr(socket, 'TCP_QUICKACK'):
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_QUICKACK, 1)
        chunk = sock.recv(size - len(result))
        if not chunk:
            raise AssertionError('unexpected disconnect')
        result.extend(chunk)
    return bytes(result)

class RelayTest(unittest.TestCase):
    def setUp(self):
        self.sockets = []
        self.child = None
        self.north_listener = self.track(listener())

    def track(self, sock):
        self.sockets.append(sock)
        sock.settimeout(5)
        return sock

    def tearDown(self):
        if self.child:
            self.child.kill()
            self.child.wait(timeout=5)
        for sock in self.sockets:
            sock.close()

    def start(self, south, mode='server', direction='both'):
        self.child = subprocess.Popen([FIXTURE, str(self.north_listener.getsockname()[1]),
                                       str(south), mode, direction])
        north = self.track(self.north_listener.accept()[0])
        self.assertEqual(receive(north, 4), b'\0REG')
        return north

    def connect(self, port):
        deadline = time.monotonic() + 5
        while True:
            try:
                return self.track(socket.create_connection(('127.0.0.1', port), timeout=1))
            except ConnectionRefusedError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.02)

    def server(self, direction='both'):
        reservation = listener()
        port = reservation.getsockname()[1]
        reservation.close()
        north = self.start(port, direction=direction)
        clients = [self.connect(port) for _ in range(3)]
        # Uplink proves each client has been accepted before testing broadcast.
        for i, client in enumerate(clients):
            payload = bytes([i, 0, 255]) * 3000
            client.sendall(payload)
            self.assertEqual(receive(north, len(payload)), payload)
        return port, north, clients

    def test_heartbeat_registration_interval_and_reconnect(self):
        reservation = listener()
        port = reservation.getsockname()[1]
        reservation.close()
        north = self.start(port, direction='heartbeat')
        client = self.connect(port)
        started = time.monotonic()
        self.assertEqual(receive(north, 4), b'\0HB\xff')
        self.assertGreater(time.monotonic() - started, .7)
        client.sendall(b'payload')
        self.assertEqual(receive(north, 7), b'payload')
        self.assertEqual(receive(north, 4), b'\0HB\xff')
        client.settimeout(.1)
        with self.assertRaises(socket.timeout):
            client.recv(1)  # Application heartbeat goes only north.
        north.close()
        reconnected = self.track(self.north_listener.accept()[0])
        self.assertEqual(receive(reconnected, 4), b'\0REG')
        started = time.monotonic()
        self.assertEqual(receive(reconnected, 4), b'\0HB\xff')
        self.assertGreater(time.monotonic() - started, .7)

    def test_heartbeat_disabled_keeps_payload_configuration(self):
        reservation = listener()
        port = reservation.getsockname()[1]
        reservation.close()
        north = self.start(port, direction='heartbeat-disabled')
        north.settimeout(1.3)
        with self.assertRaises(socket.timeout):
            north.recv(1)
        client = self.connect(port)
        client.sendall(b'raw')
        self.assertEqual(receive(north, 3), b'raw')

    def test_multi_client_broadcast_limit_reconnect(self):
        port, north, clients = self.server()
        excess = self.connect(port)
        self.assertEqual(excess.recv(1), b'')
        payload = bytes(range(256)) * 8
        north.sendall(payload)
        for client in clients:
            self.assertEqual(receive(client, len(payload)), payload)
        clients[1].close()
        time.sleep(.1)
        replacement = self.connect(port)
        replacement.sendall(b'replacement')
        self.assertEqual(receive(north, 11), b'replacement')
        north.close()
        reconnect = self.track(self.north_listener.accept()[0])
        self.assertEqual(receive(reconnect, 4), b'\0REG')
        replacement.sendall(b'after-reconnect')
        self.assertEqual(receive(reconnect, 15), b'after-reconnect')

    def test_uplink_only(self):
        _, north, clients = self.server('uplink')
        north.sendall(b'not-forwarded')
        self.assertFalse(select.select(clients, [], [], .2)[0])

    def test_large_broadcast_and_congested_debug(self):
        _, north, clients = self.server('blocked-debug')
        payload = bytes(range(256)) * 4096
        with ThreadPoolExecutor(max_workers=3) as pool:
            readers = [pool.submit(receive, client, len(payload)) for client in clients]
            north.sendall(payload)
            for reader in readers:
                self.assertEqual(reader.result(timeout=10), payload)
        # More than a status interval: failed debug delivery cannot stall forwarding.
        time.sleep(1.1)
        clients[0].sendall(b'live')
        self.assertEqual(receive(north, 4), b'live')

    def test_offline_north_buffers_without_dropping(self):
        south_listener = self.track(listener())
        port = self.north_listener.getsockname()[1]
        self.north_listener.close()
        self.child = subprocess.Popen([FIXTURE, str(port), str(south_listener.getsockname()[1]), 'client', 'both'])
        south = self.track(south_listener.accept()[0])
        south.sendall(b'offline-buffer')
        time.sleep(1.2)
        restored = self.track(socket.socket())
        restored.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        restored.bind(('127.0.0.1', port))
        restored.listen()
        north = self.track(restored.accept()[0])
        self.assertEqual(receive(north, 18), b'\0REGoffline-buffer')

    def test_slow_client_isolated(self):
        _, north, clients = self.server()
        # Leave the client unread, but avoid an artificially tiny TCP window
        # that makes draining already-buffered bytes take minutes on Linux.
        clients[2].setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 64 * 1024)
        payload = bytes(range(256)) * 32768
        chunk_size = 64 * 1024

        def send_in_chunks():
            for offset in range(0, len(payload), chunk_size):
                north.sendall(payload[offset:offset + chunk_size])
                time.sleep(.001)  # Bound ingress while fast readers drain concurrently.

        with ThreadPoolExecutor(max_workers=3) as pool:
            readers = [pool.submit(receive, client, len(payload), True) for client in clients[:2]]
            sender = pool.submit(send_in_chunks)
            sender.result(timeout=30)
            for reader in readers:
                self.assertEqual(reader.result(timeout=30), payload)

        # Prove isolation rather than merely finishing the fast-client transfer.
        clients[2].settimeout(5)
        slow_received = 0
        while True:
            if hasattr(socket, 'TCP_QUICKACK'):
                clients[2].setsockopt(socket.IPPROTO_TCP, socket.TCP_QUICKACK, 1)
            chunk = clients[2].recv(65536)
            if not chunk:
                break
            slow_received += len(chunk)
        self.assertLess(slow_received, len(payload), 'slow client received the full broadcast')

        clients[0].sendall(b'still-live')
        self.assertEqual(receive(north, 10), b'still-live')

    def test_tcp_client(self):
        south_listener = self.track(listener())
        north = self.start(south_listener.getsockname()[1], 'client')
        south = self.track(south_listener.accept()[0])
        south.sendall(b'\0device\xff')
        self.assertEqual(receive(north, 8), b'\0device\xff')
        north.sendall(b'command')
        self.assertEqual(receive(south, 7), b'command')

    def test_serial(self):
        master, slave = pty.openpty()
        try:
            north = self.start(os.ttyname(slave), 'serial')
            time.sleep(.05)
            os.write(master, b'\0serial\xff')
            self.assertEqual(receive(north, 8), b'\0serial\xff')
            north.sendall(b'command\0')
            self.assertTrue(select.select([master], [], [], 5)[0])
            self.assertEqual(os.read(master, 8), b'command\0')
        finally:
            os.close(master)
            os.close(slave)

if __name__ == '__main__':
    unittest.main()
