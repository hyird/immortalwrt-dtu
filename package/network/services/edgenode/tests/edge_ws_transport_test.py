"""用真实线帧验证原生 libev 传输；无需生产平台、数据库或设备。"""
import base64
import concurrent.futures
import hashlib
import json
import socket
import struct
import subprocess
import sys
import zlib


def exact(sock, count):
    output = bytearray()
    while len(output) < count:
        chunk = sock.recv(count - len(output))
        assert chunk, 'unexpected EOF'
        output.extend(chunk)
    return bytes(output)


def read_frame(sock):
    first, second = exact(sock, 2)
    assert second & 128, '客户端帧必须掩码'
    size = second & 127
    if size == 126:
        size = struct.unpack('!H', exact(sock, 2))[0]
    elif size == 127:
        size = struct.unpack('!Q', exact(sock, 8))[0]
    assert size <= 32768
    mask = exact(sock, 4)
    data = exact(sock, size)
    return first, bytes(value ^ mask[index % 4] for index, value in enumerate(data))


def send_frame(sock, first, data):
    header = bytes([first, len(data)]) if len(data) < 126 else bytes([first, 126]) + struct.pack('!H', len(data))
    sock.sendall(header + data)


def payload():
    random = 12345
    result = bytearray()
    for _ in range(4096):
        random ^= (random << 13) & 0xffffffff
        random ^= random >> 17
        random ^= (random << 5) & 0xffffffff
        result.append(random & 255)
    return bytes(result)


def scenario(executable, mode):
    expected = [b'ack', payload(), payload()]
    extension = {
        'plain': '',
        'reset': 'permessage-deflate; server_no_context_takeover; client_no_context_takeover',
        'takeover': 'permessage-deflate',
    }[mode]
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        listener.settimeout(12)
        process = subprocess.Popen([executable, f'ws://127.0.0.1:{listener.getsockname()[1]}/edge/connect'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            with listener.accept()[0] as sock:
                sock.settimeout(12)
                request = bytearray()
                while not request.endswith(b'\r\n\r\n'):
                    request.extend(exact(sock, 1))
                    assert len(request) < 8192
                headers = dict(line.split(':', 1) for line in request.decode().split('\r\n')[1:] if ':' in line)
                headers = {key.lower(): value.strip() for key, value in headers.items()}
                assert headers['sec-websocket-extensions'] == 'permessage-deflate'
                assert 'sec-websocket-protocol' not in headers
                accept = base64.b64encode(hashlib.sha1((headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
                response = f'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n'
                if extension:
                    response += f'Sec-WebSocket-Extensions: {extension}\r\n'
                sock.sendall((response + '\r\n').encode())
                send_frame(sock, 0x89, b'probe')
                decoder = zlib.decompressobj(-15)
                encoder = zlib.compressobj(9, zlib.DEFLATED, -15)
                reference = zlib.compressobj(9, zlib.DEFLATED, -15)
                lengths = []
                pong = False
                message = bytearray()
                index = 0
                while True:
                    first, data = read_frame(sock)
                    opcode = first & 15
                    if opcode >= 8:
                        assert not first & 0x40 and first & 0x80
                        if opcode == 10:
                            assert data == b'probe'
                            pong = True
                            continue
                        assert opcode == 8 and index == 3 and pong
                        send_frame(sock, 0x88, data)
                        break
                    assert index < 3
                    if not message:
                        assert opcode == 2
                        assert bool(first & 0x40) == bool(extension)
                    else:
                        assert opcode == 0 and not first & 0x40
                    message.extend(data)
                    if not first & 0x80:
                        continue
                    wire = bytes(message)
                    message.clear()
                    lengths.append(len(wire))
                    if extension:
                        assert decoder.decompress(wire + b'\x00\x00\xff\xff') == expected[index]
                        reference_wire = reference.compress(expected[index]) + reference.flush(zlib.Z_SYNC_FLUSH)
                        assert wire == reference_wire[:-4], '出站字节与 level 9 编码不一致'
                        reply = encoder.compress(expected[index]) + encoder.flush(zlib.Z_SYNC_FLUSH)
                        reply = reply[:-4]
                    else:
                        assert wire == expected[index]
                        reply = wire
                    # 压缩消息跨分片，同时夹入控制帧。
                    cut = max(1, len(reply) // 2)
                    send_frame(sock, 0x02 | (0x40 if extension else 0), reply[:cut])
                    send_frame(sock, 0x89, b'probe')
                    send_frame(sock, 0x80, reply[cut:])
                    if mode == 'reset':
                        decoder = zlib.decompressobj(-15)
                        encoder = zlib.compressobj(9, zlib.DEFLATED, -15)
                        reference = zlib.compressobj(9, zlib.DEFLATED, -15)
                    index += 1
                if mode == 'takeover':
                    assert lengths[2] < lengths[1] // 4, lengths
                if mode == 'reset':
                    assert lengths[1] == lengths[2]
            stdout, stderr = process.communicate(timeout=12)
            assert process.returncode == 0, stderr.decode()
            return {'mode': mode, 'payload_bytes': [len(item) for item in expected], 'wire_payload_bytes': lengths, 'result': 'passed'}
        except Exception:
            print(f'{mode}: received={locals().get("index")} sizes={locals().get("lengths")}', file=sys.stderr)
            if process.poll() is None:
                process.kill()
            _, diagnostic = process.communicate()
            print(f'{mode} exit={process.returncode}: {diagnostic.decode()}', file=sys.stderr)
            raise
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()


if __name__ == '__main__':
    # 并行连接验证测试实例不共享压缩状态；进程内多平台隔离需另行集成验收。
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        results = list(pool.map(lambda mode: scenario(sys.argv[1], mode), ['plain', 'reset', 'takeover']))
    print(json.dumps(results, ensure_ascii=False, indent=2))
