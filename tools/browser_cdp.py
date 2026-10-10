"""Small, local-only CDP transport for real-time media tests; standard library only."""
import base64
import json
import os
import socket
import struct
from urllib.parse import urlsplit


class BrowserSocket:
    def __init__(self, url):
        address = urlsplit(url)
        if address.hostname not in ('localhost', '127.0.0.1'):
            raise ValueError('Only local browser debugging is supported')
        self.socket = socket.create_connection((address.hostname, address.port), timeout=45)
        self.reader = self.socket.makefile('rb')
        key = base64.b64encode(os.urandom(16)).decode()
        self.socket.sendall((f'GET {address.path} HTTP/1.1\r\nHost: {address.netloc}\r\n'
                             f'Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n'
                             'Sec-WebSocket-Version: 13\r\n\r\n').encode())
        status = self.reader.readline()
        if b'101' not in status:
            raise RuntimeError(f'Browser websocket rejected: {status!r}')
        while self.reader.readline() != b'\r\n':
            pass
        self.sequence = 0

    def send(self, payload, opcode=1):
        mask = os.urandom(4)
        size = len(payload)
        header = bytes([0x80 | opcode])
        header += bytes([0x80 | size]) if size < 126 else (bytes([0xfe]) + struct.pack('!H', size) if size < 65536 else bytes([0xff]) + struct.pack('!Q', size))
        self.socket.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def receive(self):
        chunks = []
        while True:
            header = self.reader.read(2)
            if len(header) != 2:
                raise RuntimeError('Browser disconnected')
            size = header[1] & 127
            if size == 126:
                size = struct.unpack('!H', self.reader.read(2))[0]
            elif size == 127:
                size = struct.unpack('!Q', self.reader.read(8))[0]
            payload = self.reader.read(size)
            opcode = header[0] & 15
            if opcode == 8:
                raise RuntimeError('Browser closed connection')
            if opcode == 9:
                self.send(payload, 10)
                continue
            chunks.append(payload)
            if header[0] & 128:
                return json.loads(b''.join(chunks))

    def call(self, method, **params):
        self.sequence += 1
        self.send(json.dumps(dict(id=self.sequence, method=method, params=params)).encode())
        while True:
            reply = self.receive()
            if reply.get('id') == self.sequence:
                if 'error' in reply:
                    raise RuntimeError(reply['error'])
                return reply.get('result', {})

    def close(self):
        self.reader.close()
        self.socket.close()
