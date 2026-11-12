"""Replace a mouse between guest samples; unchanged CCS must not hide hotplug."""
from pathlib import Path
import subprocess
import sys
import time
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]
smoke.IMAGE = ROOT / '.storage-test/root-replacement.img'
smoke.IMAGE.write_bytes((ROOT / '.storage-test/root.img').read_bytes())
q = smoke.Qemu(input_devices=False, extra_args=(
    '-device', 'usb-mouse,id=swap,bus=xhci.0,port=2',
    '-device', 'usb-kbd,id=steady,bus=xhci.0,port=3'))
symbols = subprocess.check_output(['nm','-n',str(ROOT/'wyzar')], text=True)
mouse_x = next(int(line.split()[0],16) for line in symbols.splitlines() if line.split()[-1] == 'g_mouse_x')

def mouse_position():
    path = ROOT / '.storage-test/replacement-mouse.bin'
    assert not q.monitor(f'memsave 0x{mouse_x:x} 4 "{path}"')
    return int.from_bytes(path.read_bytes(), 'little', signed=True)

def move_new_mouse():
    mouse = next(m for m in q.command('query-mice') if 'HID' in m['name'] or 'USB' in m['name'])
    assert not q.monitor(f"mouse_set {mouse['index']}")
    before = mouse_position()
    assert not q.monitor('mouse_move 17 0')
    time.sleep(.4)
    assert mouse_position() > before, 'replacement mouse was not configured/rearmed'

def wait_log(label, marker, count):
    for _ in range(100):
        time.sleep(.2)
        log = q.capture(label)
        if log.count(marker) >= count: return log
    raise AssertionError('missing replacement: ' + log[-3000:])

try:
    for _ in range(100):
        time.sleep(.5)
        try: log = q.capture('root-replacement-start')
        except AssertionError as error:
            if 'Invalid addr' in str(error): continue
            raise
        if '[wm] Shell task started inside WM frame.' in log: break
    else: raise AssertionError('boot timeout')
    assert '[storage] WYZARDATA mounted as root /.' in log
    move_new_mouse()
    for cycle in range(5):
        # Freeze the guest so both physical changes occur before its next read.
        # QEMU latches CSC while final CCS remains connected.
        q.command('stop')
        q.command('device_del', {'id':'swap'})
        q.command('device_add', {'driver':'usb-mouse', 'id':'swap', 'bus':'xhci.0',
                                 'port':'2', 'usb_version':1 if cycle % 2 == 0 else 2})
        q.command('cont')
        wait_log('root-replacement-fast', '[usb] Root device replacement; port:', cycle + 1)
        time.sleep(.3)
        move_new_mouse()
        q.type(f'write /swap{cycle}.txt disk-survived-swap\n', pause=.5)
    q.command('device_del', {'id':'swap'})
    wait_log('root-replacement-empty', '[usb] Root connection change; port:', 1)
    time.sleep(5)
    q.command('device_add', {'driver':'usb-mouse', 'id':'swap', 'bus':'xhci.0', 'port':'2'})
    wait_log('root-replacement-delayed', '[usb] Root connection change; port:', 2)
    time.sleep(.3)
    move_new_mouse()
    q.type('write /delayed.txt disk-survived-delay\n', pause=.5)
    time.sleep(.5)
    log = q.capture('root-replacement-end')
    assert log.count('xHCI controller running.') == 1
    assert log.count('[usb] Root device replacement; port:') == 5
    assert 'KERNEL PANIC' not in log
finally:
    q.close()
sys.path.insert(0, str(ROOT / 'tools'))
from system_volume import SystemVolume
from make_usb_image import START, BOOT_SECTORS, DATA_SECTORS
raw = smoke.IMAGE.read_bytes()
offset = (START + BOOT_SECTORS) * 512
data = SystemVolume(raw[offset:offset + DATA_SECTORS * 512])
for cycle in range(5): assert data.read(f'swap{cycle}.txt') == b'disk-survived-swap'
assert data.read('delayed.txt') == b'disk-survived-delay'
print('Five coalesced mouse replacements and a 5-second-empty replacement: mouse, keyboard and disk survived')
