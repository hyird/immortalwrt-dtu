"""Exercise real TCP framing, compact views, migration and zero-filled ranges."""
import contextlib
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


def request(transaction, function=4, address=0, count=10, unit=1):
    return struct.pack(">HHHBBHH", transaction, 0, 6, unit, function, address, count)


@contextlib.contextmanager
def fixture(directory, mode):
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    process = subprocess.Popen([str(pathlib.Path(sys.argv[1]).resolve()), str(port), mode],
                               cwd=directory, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        assert select.select([process.stdout], [], [], 5)[0], "listener did not start"
        assert process.stdout.readline() == b"READY\n", process.communicate(timeout=2)
        yield port
    finally:
        process.terminate()
        process.communicate(timeout=5)


def read(connection, function, address, count):
    connection.sendall(request(20, function, address, count))
    response = receive_adu(connection)
    assert response[:4] == struct.pack(">HH", 20, 0)
    assert response[7] == function, response
    expected_size = (count + 7) // 8 if function in (1, 2) else count * 2
    assert response[8] == expected_size and len(response) == 9 + expected_size
    return response[9:]


def exception(connection, function, address, count, code):
    connection.sendall(request(21, function, address, count))
    assert receive_adu(connection)[7:] == bytes([function | 0x80, code])


def ignored_write(connection, function, address, count, data):
    body = (struct.pack(">BHH", function, address, int.from_bytes(data, "big"))
            if function in (5, 6) else
            struct.pack(">BHHB", function, address, count, len(data)) + data)
    connection.sendall(struct.pack(">HHHB", 23, 0, len(body) + 1, 1) + body)
    assert receive_adu(connection) == struct.pack(">HHHB", 23, 0, 6, 1) + body[:5]


pathlib.Path("build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="edge-gateway-", dir="build") as temporary:
    directory = pathlib.Path(temporary).resolve()
    # The same map survives process restarts, disable/delete/empty configurations,
    # rejected saves and an explicitly persisted permutation.
    for mode, sorted_view in [("normal", False), ("normal", False),
                              ("derived-failure", False), ("disabled", False), ("deleted", False), ("empty", False),
                              ("save-failure", False), ("sorted", True), ("normal", True)]:
        with fixture(directory, mode) as port:
            empty = mode in {"disabled", "deleted", "empty"}
            native = (struct.pack(">QdIH", 0xffffffffffffffff, 1.5, 0x12345678, 0x1234)
                      if sorted_view else
                      struct.pack(">QHId", 0xffffffffffffffff, 0x1234, 0x12345678, 1.5))
            native += struct.pack(">d", 0.0 if mode == "derived-failure" else 3.0)
            flags = b"\x01\x00" if mode == "derived-failure" else b"\x03\x00"
            with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
                packet = request(7)
                # Retain every fragment, then pipeline later frames before reading.
                for byte in packet:
                    connection.sendall(bytes([byte]))
                connection.sendall(request(8, function=3) + request(9, function=0x7f))
                response = receive_adu(connection)
                assert response[:9] == struct.pack(">HHHBBB", 7, 0, 23, 1, 4, 20)
                assert response[9:] == (b"\x00" * 20 if empty else native[:20])
                assert receive_adu(connection)[7] == 3
                assert receive_adu(connection)[7:] == b"\xff\x01"
                if empty:
                    assert read(connection, 3, 0, 125) == b"\x00" * 250
                    assert read(connection, 2, 0, 2000) == b"\x00" * 250
                    assert read(connection, 4, 0, 60) == b"\x00" * 120
                else:
                    assert read(connection, 3, 0, 19) == native + b"\x00" * 8
                    assert read(connection, 3, 2, 16) == native[4:] + b"\x00" * 6
                    assert read(connection, 2, 0, 10) == flags
                    assert read(connection, 2, 1, 10) == bytes([flags[0] >> 1, 0])
                    assert read(connection, 1, 0, 10) == flags
                    assert read(connection, 4, 0, 19) == native + b"\x00" * 8
                # All fixture points are source-read-only: writes acknowledge
                # normally without replacing actual samples or filling holes.
                ignored_write(connection, 5, 0, 1, b"\x00\x00")
                ignored_write(connection, 6, 4, 1, b"\xab\xcd")
                ignored_write(connection, 15, 0, 10, b"\x00\x00")
                ignored_write(connection, 16, 0, 3, b"\x00" * 6)
                ignored_write(connection, 6, 11, 1, b"\xff\xff")
                ignored_write(connection, 5, 1, 1, b"\x00\x00")
                ignored_write(connection, 16, 11, 4, b"\x7f\xf8" + b"\x00" * 6)
                ignored_write(connection, 6, 65535, 1, b"\x12\x34")
                assert read(connection, 3, 0, 19) == (b"\x00" * 38 if empty else native + b"\x00" * 8)
                assert read(connection, 2, 0, 10) == (b"\x00\x00" if empty else flags)
                for function in (1, 2, 3, 4):
                    bits = function in (1, 2)
                    assert read(connection, function, 65535, 1) == b"\x00" * (1 if bits else 2)
                    assert read(connection, function, 65411, 125) == b"\x00" * (16 if bits else 250)
                    exception(connection, function, 65535, 2, 2)
                    exception(connection, function, 0, 0, 3)
                    exception(connection, function, 0, 2001 if bits else 126, 3)
                    # Well-framed ADU with the wrong PDU length is exception 03.
                    connection.sendall(struct.pack(">HHHBBH", 22, 0, 4, 1, function, 0))
                    assert receive_adu(connection)[7:] == bytes([function | 0x80, 3])
                # An idle client does not block another connection.
                with socket.create_connection(("127.0.0.1", port), timeout=3) as other:
                    assert read(other, 3, 65535, 1) == b"\x00\x00"
            with socket.create_connection(("127.0.0.1", port), timeout=3) as malformed:
                malformed.sendall(struct.pack(">HHH", 1, 0, 65535))
                assert malformed.recv(1) == b""
    with fixture(directory, "unit247") as port:
        for function in (1, 2, 3, 4, 5, 6, 15, 16, 0x7f):
            body = (struct.pack(">BHHB", function, 0, 1, 1 if function == 15 else 2)
                    + (b"\x00" if function == 15 else b"\x00\x01")
                    if function in (15, 16) else
                    struct.pack(">BHH", function, 0, 1 if function not in (5, 6) else 0))
            with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
                connection.sendall(struct.pack(">HHHB", 123, 0, len(body) + 1, 247) + body)
                response = receive_adu(connection)
                assert response[:4] == struct.pack(">HH", 123, 0) and response[6] == 247
            for wrong in (0, 1, 246, 248, 255):
                with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
                    connection.sendall(struct.pack(">HHHB", 124, 0, len(body) + 1, wrong) + body)
                    assert connection.recv(1) == b""
    # A corrupt persisted table must not produce a fabricated usable mapping.
    with (directory / "network.map").open("ab") as mapping:
        mapping.write(b"corrupt\n")
    failed = subprocess.run([str(pathlib.Path(sys.argv[1]).resolve()), "1502", "normal"],
                            cwd=directory, capture_output=True, timeout=3)
    assert failed.returncode == 3 and failed.stdout == b""
