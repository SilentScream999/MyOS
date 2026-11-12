"""Hotplug the only keyboard on the boot disk's xHCI; keep disk and mouse alive."""
from pathlib import Path
import subprocess
import socket
import sys
import time
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]
smoke.IMAGE = ROOT / '.storage-test/root-hotplug.img'
smoke.IMAGE.write_bytes((ROOT / '.storage-test/root.img').read_bytes())
q = smoke.Qemu(input_devices=False, extra_args=(
    '-device', 'usb-mouse,id=steady,bus=xhci.0,port=3', '-gdb', 'tcp:127.0.0.1:45456',
    '-qtest', 'tcp:127.0.0.1:45457,server=on,wait=off'))
symbols = subprocess.check_output(['nm', '-n', str(ROOT / 'wyzar')], text=True)
mouse_x = next(int(line.split()[0], 16) for line in symbols.splitlines() if line.split()[-1] == 'g_mouse_x')

class DebugMemory:
    """Minimal GDB remote memory access to this disposable QEMU instance."""
    def __init__(self):
        self.sock = socket.create_connection(('127.0.0.1', 45456), timeout=3)
        self.request('?')
    def request(self, payload):
        data = payload.encode()
        self.sock.sendall(b'$' + data + b'#' + f'{sum(data) & 255:02x}'.encode())
        while self.sock.recv(1) != b'$': pass
        response = b''
        while True:
            byte = self.sock.recv(1)
            if byte == b'#': break
            assert byte, 'debugger connection closed'
            response += byte
        checksum = self.sock.recv(2)
        assert int(checksum, 16) == sum(response) & 255
        self.sock.sendall(b'+')
        return response.decode()
    def read(self, address, length):
        return bytes.fromhex(self.request(f'm{address:x},{length:x}'))
    def write(self, address, data):
        assert self.request(f'M{address:x},{len(data):x}:{data.hex()}') == 'OK'
    def close(self):
        self.request('D')
        self.sock.close()

def symbol(suffix):
    return next(int(line.split()[0],16) for line in symbols.splitlines() if line.split()[-1].endswith(suffix))

def read_x():
    path = ROOT / '.storage-test/root-hotplug-mouse.bin'
    assert not q.monitor(f'memsave 0x{mouse_x:x} 4 "{path}"')
    return int.from_bytes(path.read_bytes(), 'little', signed=True)

def wait_scan(count, label):
    for _ in range(100):
        time.sleep(.2)
        log = q.capture(label)
        if log.count('[boot] USB discovery milliseconds:') >= count: return log
    raise AssertionError('hotplug did not finish: ' + log[-3500:])

try:
    for _ in range(100):
        time.sleep(.5)
        try: log = q.capture('root-hotplug-start')
        except AssertionError as error:
            if 'Invalid addr' in str(error): continue
            raise
        if '[wm] Shell task started inside WM frame.' in log: break
    else: raise AssertionError('boot timeout')
    assert '[storage] WYZARDATA mounted as root /.' in log
    assert 'No keyboard found yet.' in log
    mice = q.command('query-mice')
    usb_mouse = next(mouse for mouse in mice if 'USB' in mouse['name'] or 'HID' in mouse['name'])
    assert not q.monitor(f"mouse_set {usb_mouse['index']}")
    scans = 1
    for cycle in range(5):
        # Alternate USB 1.1 full-speed and USB 2 high-speed enumeration.
        q.command('device_add', {'driver':'usb-kbd', 'id':'keyboard', 'bus':'xhci.0',
                                 'port':'2', 'usb_version':1 if cycle % 2 == 0 else 2})
        scans += 1
        wait_scan(scans, 'root-hotplug-added')
        time.sleep(.3)
        q.type(f'write /plug{cycle}.txt keyboard-and-disk-alive\n', pause=.5)
        before = read_x()
        assert not q.monitor('mouse_move 13 0')
        time.sleep(.3)
        assert read_x() > before, 'unchanged USB mouse stopped after keyboard attach'
        q.command('device_del', {'id':'keyboard'})
        scans += 1
        wait_scan(scans, 'root-hotplug-removed')
        before = read_x()
        assert not q.monitor('mouse_move 13 0')
        time.sleep(.3)
        assert read_x() > before, 'unchanged USB mouse stopped after keyboard removal'
    # Simulate a timed-out reset whose port was already counted as connected:
    # PED becomes ready later with no new CCS transition. No debugger needed on
    # the host; QEMU's private remote-memory interface supplies the missed reset.
    q.command('stop')
    q.command('device_add', {'driver':'usb-kbd', 'id':'late', 'bus':'xhci.0',
                             'port':'2', 'usb_version':1})
    debug = DebugMemory()
    try:
        debug.write(symbol('usb_root_watch') + 5 * 16, b'\x01\x01')
        debug.write(symbol('usb_root_attempted') + 5, b'\x00')
        # Debug writes deliberately bypass MMIO side effects. Use qtest for the
        # actual port-reset register write, just as the CPU driver would.
        device = next(d for bus in q.command('query-pci') for d in bus['devices'] if d.get('qdev_id') == 'xhci')
        bar = next(r['address'] for r in device['regions'] if r['bar'] == 0)
        with socket.create_connection(('127.0.0.1', 45457), timeout=3) as test_socket:
            stream = test_socket.makefile('rwb')
            def qt(command):
                stream.write(command.encode() + b'\n'); stream.flush()
                result = stream.readline().decode().strip()
                assert result.startswith('OK'), result
                return int(result.split()[1], 16) if len(result.split()) > 1 else 0
            caplen = qt(f'readb {bar:#x}')
            portsc = bar + caplen + 0x400 + 5 * 0x10
            sc = qt(f'readl {portsc:#x}')
            assert sc & 1
            neutral = sc & ((15 << 5) | (1 << 9) | (3 << 14) | (7 << 25))
            # The simulated earlier connection handling acknowledged CSC; only
            # reset completion/late PED should remain for the readiness watcher.
            qt(f'writel {portsc:#x} {neutral | (1 << 4) | (sc & (127 << 17)):#x}')
            completed = qt(f'readl {portsc:#x}')
            assert completed & 3 == 3
            # QEMU can latch CSC again when the reset updates the port. Model
            # connection handling as complete, leaving only readiness to detect.
            neutral = completed & ((15 << 5) | (1 << 9) | (3 << 14) | (7 << 25))
            qt(f'writel {portsc:#x} {neutral | (1 << 17):#x}')
            assert not (qt(f'readl {portsc:#x}') & (1 << 17))
        debug.write(symbol('usb_root_changes') + 5 * 16, b'\x00')
    finally: debug.close()
    q.command('cont')
    scans += 1
    log = wait_scan(scans, 'root-hotplug-late')
    assert '[usb] Late-ready root port:' in log
    time.sleep(.3)
    q.type('write /late.txt late-keyboard-working\n', pause=.5)
    log = q.capture('root-hotplug-end')
    assert log.count('xHCI controller running.') == 1, log[-3500:]
    assert log.count('[usb] Root connection change; port:') == 10
    assert 'KERNEL PANIC' not in log
finally:
    q.close()
sys.path.insert(0, str(ROOT / 'tools'))
from system_volume import SystemVolume
from make_usb_image import START, BOOT_SECTORS, DATA_SECTORS
raw = smoke.IMAGE.read_bytes()
offset = (START + BOOT_SECTORS) * 512
data = SystemVolume(raw[offset:offset + DATA_SECTORS * 512])
for cycle in range(5):
    assert data.read(f'plug{cycle}.txt') == b'keyboard-and-disk-alive'
assert data.read('late.txt') == b'late-keyboard-working'
print('Five root keyboard attach/removal cycles plus missed reset completion: keyboard, mouse and disk survived; no xHCI reset')
