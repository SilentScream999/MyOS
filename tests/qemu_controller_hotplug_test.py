"""Reset a second xHCI repeatedly; verify first-controller disk and HID survive."""
import time
from pathlib import Path
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]
smoke.IMAGE = ROOT / '.storage-test/controller-hotplug.img'
smoke.IMAGE.write_bytes((ROOT / '.storage-test/root.img').read_bytes())
q = smoke.Qemu(extra_args=('-device', 'qemu-xhci,id=spare'))
try:
    for _ in range(100):
        time.sleep(.5)
        try:
            log = q.capture('controller-hotplug-start')
        except AssertionError as error:
            if 'Invalid addr' in str(error): continue
            raise
        if '[wm] Shell task started inside WM frame.' in log: break
    else: raise AssertionError('boot timeout')
    assert '[storage] WYZARDATA mounted as root /.' in log
    assert log.count('xHCI controller running.') == 2
    scans = log.count('[boot] USB discovery milliseconds:')
    # An idle second controller is not allowed to upset the root disk.
    for cycle in range(3):
        q.command('device_add', {'driver':'usb-mouse', 'id':'probe', 'bus':'spare.0', 'port':'1'})
        for _ in range(80):
            time.sleep(.2)
            log = q.capture('controller-hotplug-add')
            if log.count('[boot] USB discovery milliseconds:') > scans: break
        else: raise AssertionError('insert did not trigger enumeration')
        scans += 1
        q.command('device_del', {'id':'probe'})
        for _ in range(80):
            time.sleep(.2)
            log = q.capture('controller-hotplug-remove')
            if log.count('[boot] USB discovery milliseconds:') > scans: break
        else: raise AssertionError('remove did not trigger enumeration')
        scans += 1
        time.sleep(.5)
        q.type(f'write /ctrl{cycle}.txt surviving-disk\n', pause=.5)
    time.sleep(2)
    log = q.capture('controller-hotplug-end')
    assert 'KERNEL PANIC' not in log
    assert log.count('xHCI controller running.') == 2, log[-6000:]
    assert log.count('[usb] Root connection change; port:') == 6
finally:
    q.close()

import sys
sys.path.insert(0, str(ROOT / 'tools'))
from system_volume import SystemVolume
from make_usb_image import START, BOOT_SECTORS, DATA_SECTORS
raw = smoke.IMAGE.read_bytes()
offset = (START + BOOT_SECTORS) * 512
data = SystemVolume(raw[offset:offset + DATA_SECTORS * 512])
for cycle in range(3):
    assert data.read(f'ctrl{cycle}.txt') == b'surviving-disk'
print('Two controllers: 3 insert/removal cycles, keyboard and disk writes survived')
