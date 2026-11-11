#!/usr/bin/env python3
"""A valid USB disk with a bad root marker must report topology only on failure."""
from pathlib import Path
import shutil
import subprocess
import time
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.storage-test'


def main():
    image = OUT / 'usb-recovery-report.img'
    partition = OUT / 'usb-recovery-partition.img'
    offset = (2048 + 262144) * 512
    shutil.copyfile(ROOT / 'wyzar-usb.img', image)
    with image.open('rb') as source:
        source.seek(offset)
        partition.write_bytes(source.read())
    marker = OUT / 'bad-root-marker'
    marker.write_bytes(b'INVALID\n')
    manifest = OUT / 'bad-root-manifest'
    manifest.write_text(f'put /etc/root.cfg {marker}\n')
    subprocess.run([str(OUT / 'ext4-image-tool'), str(partition), str(manifest)], check=True)
    with image.open('r+b') as destination:
        destination.seek(offset)
        destination.write(partition.read_bytes())
    smoke.IMAGE = image
    # This fixture tests OS root validation, independently of OVMF's removable
    # media boot selection through a nested emulated hub.
    smoke.BOOT_ISO = True
    q = smoke.Qemu(hub_depth=2)
    try:
        log = ''
        for attempt in range(90):
            time.sleep(1)
            try:
                log = q.capture('usb-recovery-report')
            except AssertionError as error:
                if 'Invalid addr' in str(error):
                    continue
                raise
            if '[init] Decoupled terminal boot.' in log:
                break
        else:
            raise AssertionError(log[-6000:])
        assert '[storage] No unique valid system volume; RAM recovery mode.' in log
        assert '[storage] Discovery: controllers=1 hubs=2 disks=1 online=1 keyboards=1' in log, log[-6000:]
        assert log.count('[storage] Hub:') == 2 and log.count('[storage] Disk:') == 1
        assert 'vid:pid=' in log and 'milliseconds:' not in log
        assert '[storage] WYZARDATA mounted as root /.' not in log
        q.type('cat /run/storage.txt\n')
        q.capture('usb-recovery-report-shell')
        print('QEMU recovery: failure-only topology, detected disks, IDs, quiet defaults and RAM shell passed', flush=True)
    finally:
        q.close()


if __name__ == '__main__':
    main()
