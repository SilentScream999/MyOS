"""Small GDB remote client for disposable emulator fault-injection fixtures."""
import socket

class DebugMemory:
    def __init__(self):
        self.sock = socket.create_connection(('127.0.0.1',45456), timeout=15)
        self.request('?')
    def byte(self):
        value = self.sock.recv(1)
        assert value, 'debugger connection closed'
        return value
    def request(self, payload):
        data = payload.encode()
        self.sock.sendall(b'$'+data+b'#'+f'{sum(data)&255:02x}'.encode())
        while self.byte() != b'$': pass
        data = b''
        while True:
            value = self.byte()
            if value == b'#': break
            data += value
        checksum = self.byte()+self.byte()
        assert int(checksum,16) == sum(data)&255
        self.sock.sendall(b'+')
        return data.decode()
    def read(self, address, length):
        return bytes.fromhex(self.request(f'm{address:x},{length:x}'))
    def write(self, address, data):
        assert self.request(f'M{address:x},{len(data):x}:{data.hex()}') == 'OK'
    def close(self):
        self.request('D')
        self.sock.close()
