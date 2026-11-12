"""Exercise actual TCP framing and backpressure, using only the standard library."""
import pathlib
import select
import socket
import struct
import subprocess
import sys
import tempfile


def receive_exact(connection, count):
    output = bytearray()
    while len(output) < count:
        chunk = connection.recv(count - len(output))
        assert chunk, "unexpected connection close"
        output.extend(chunk)
    return bytes(output)


def receive_adu(connection):
    header = receive_exact(connection, 6)
    return header + receive_exact(connection, struct.unpack(">H", header[4:6])[0])


def request(transaction, function=4, address=0, count=10):
    return struct.pack(">HHHBBHH", transaction, 0, 6, 1, function, address, count)


with socket.socket() as reservation:
    reservation.bind(("127.0.0.1", 0))
    port = reservation.getsockname()[1]
with tempfile.TemporaryDirectory(prefix="edge-gateway-") as temporary:
    process = subprocess.Popen([str(pathlib.Path(sys.argv[1]).resolve()), str(port)],
                               cwd=temporary, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        assert select.select([process.stdout], [], [], 5)[0], "listener did not start"
        assert process.stdout.readline() == b"READY\n"
        with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
            packet = request(7)
            # Every fragment must be retained; pipeline later frames before reading responses.
            for byte in packet:
                connection.sendall(bytes([byte]))
            connection.sendall(request(8, function=3) + request(9, function=6))
            response = receive_adu(connection)
            assert response[:9] == struct.pack(">HHHBBB", 7, 0, 23, 1, 4, 20)
            assert response[13:21] == b"\xff" * 8
            assert receive_adu(connection)[7] == 3
            assert receive_adu(connection) == struct.pack(">HHHBBB", 9, 0, 3, 1, 0x86, 1)
            # A client delaying its next request must not block another client.
            with socket.create_connection(("127.0.0.1", port), timeout=3) as other:
                other.sendall(request(10, address=65535, count=2))
                assert receive_adu(other)[7:] == b"\x84\x02"
            connection.sendall(request(11, count=126))
            assert receive_adu(connection)[7:] == b"\x84\x03"
        with socket.create_connection(("127.0.0.1", port), timeout=3) as malformed:
            malformed.sendall(struct.pack(">HHH", 1, 0, 65535))
            assert malformed.recv(1) == b""
    finally:
        process.terminate()
        process.communicate(timeout=5)
