#!/usr/bin/env python3
"""A disk appearing during startup gets root; post-start hotplug never swaps it."""
from pathlib import Path
import shutil
import time
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.storage-test'


def wait_log(q, label, predicate):
    log = ''
    for _ in range(600):
        time.sleep(.05)
        try:
            log = q.capture(label)
        except AssertionError as error:
            if 'Invalid addr' in str(error):
                continue
            raise
        if predicate(log):
            return log
    raise AssertionError(label + ': ' + log[-6000:])


def attach(q, image, port):
    q.command('blockdev-add', {'node-name': 'late-system-disk', 'driver': 'raw',
                             'file': {'driver': 'file', 'filename': str(image)}})
    q.command('device_add', {'driver': 'usb-storage', 'id': 'late-stick',
                            'drive': 'late-system-disk', 'bus': 'xhci.0', 'port': port})


def main():
    smoke.BOOT_ISO = True
    for during_startup, hub_depth in ((True, 0), (True, 2), (False, 0)):
        image = OUT / 'late-system-disk.img'
        shutil.copyfile(ROOT / 'wyzar-usb.img', image)
        label = 'disk-during-startup' if during_startup else 'disk-after-recovery'
        if hub_depth:
            label += '-nested-hubs'
        q = smoke.Qemu(storage=False, hub_depth=hub_depth)
        port = '1' + '.1' * hub_depth if hub_depth else '3'
        try:
            if during_startup:
                wait_log(q, label, lambda log: '[storage] Waiting for USB system storage discovery.' in log)
                attach(q, image, port)
                log = wait_log(q, label, lambda log: '[init] Decoupled terminal boot.' in log)
                assert '[storage] WYZARDATA mounted as root /.' in log, log[-6000:]
                assert '[storage] ext4 journal active on USB system root.' in log
                assert '[wallpaper] Loaded disk asset into RAM.' in log
                assert '[storage] No unique valid system volume; RAM recovery mode.' not in log
            else:
                wait_log(q, label, lambda log: '[init] Decoupled terminal boot.' in log)
                attach(q, image, port)
                log = wait_log(q, label, lambda log: '[storage] USB SCSI disk ready (512-byte sectors).' in log)
                assert '[storage] No unique valid system volume; RAM recovery mode.' in log
                assert '[storage] WYZARDATA mounted as root /.' not in log
                assert log.index('[storage] No unique valid system volume; RAM recovery mode.') < log.index('[storage] USB SCSI disk ready')
                assert 'disks=0 online=0' in log
            assert 'milliseconds:' not in log
            print(label + ': passed', flush=True)
        finally:
            q.close()


if __name__ == '__main__':
    main()
