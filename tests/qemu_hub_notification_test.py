"""A brief hub connection bounce must not restart xHCI or offline the root disk."""
import time
from pathlib import Path
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]
smoke.IMAGE = ROOT / '.storage-test/hub-notification.img'
smoke.IMAGE.write_bytes((ROOT / '.storage-test/root.img').read_bytes())
q = smoke.Qemu(hub_depth=1)
try:
    for _ in range(100):
        time.sleep(.5)
        try:
            log = q.capture('hub-notification-start')
        except AssertionError as error:
            if 'Invalid addr' in str(error): continue
            raise
        if '[wm] Shell task started inside WM frame.' in log: break
    else: raise AssertionError('boot timeout')
    assert '[storage] WYZARDATA mounted as root /.' in log
    assert 'Queued status-change TRB for hub slot:' in log
    assert '[usb] Hub status watch ready.' in log
    for cycle in range(5):
        q.command('device_add', {'driver':'usb-mouse', 'id':'bounce', 'bus':'xhci.0', 'port':'1.2'})
        time.sleep(.02)
        q.command('device_del', {'id':'bounce'})
        time.sleep(1)
        q.type(f'write /bounce{cycle}.txt disk-still-online\n', pause=.5)
    log = q.capture('hub-notification-end')
    assert log.count('xHCI controller running.') == 1, log[-5000:]
    assert '[usb] Stable hub port change' not in log
    assert '[usb] Hub notification received; checking port status.' in log
    assert 'KERNEL PANIC' not in log
    # Lasting changes must still be discovered without resetting the controller.
    q.command('device_add', {'driver':'usb-mouse', 'id':'lasting', 'bus':'xhci.0', 'port':'1.2'})
    for _ in range(100):
        time.sleep(.2)
        log = q.capture('hub-notification-stable-add')
        if log.count('[boot] USB discovery milliseconds:') == 2: break
    else: raise AssertionError('stable hub insertion was not enumerated: ' + log[-2500:])
    q.command('device_del', {'id':'lasting'})
    for _ in range(100):
        time.sleep(.2)
        log = q.capture('hub-notification-stable-remove')
        if log.count('[boot] USB discovery milliseconds:') == 3: break
    else: raise AssertionError('stable hub removal was not enumerated: ' + log[-2500:])
    time.sleep(1)
    log = q.capture('hub-notification-stable-end')
    assert log.count('xHCI controller running.') == 1
finally:
    q.close()

import sys
sys.path.insert(0, str(ROOT / 'tools'))
from system_volume import SystemVolume
from make_usb_image import START, BOOT_SECTORS, DATA_SECTORS
raw = smoke.IMAGE.read_bytes()
offset = (START + BOOT_SECTORS) * 512
data = SystemVolume(raw[offset:offset + DATA_SECTORS * 512])
for cycle in range(5):
    assert data.read(f'bounce{cycle}.txt') == b'disk-still-online'
print('Five hub-port bounces: disk/HID survived; stable hub insert/removal still detected')
