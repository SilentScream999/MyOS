"""Verify splash-to-wallpaper handoff and actual userspace log metadata."""
from pathlib import Path
import re
import struct
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from system_volume import SystemVolume
from make_usb_image import START, BOOT_SECTORS, DATA_SECTORS
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]
OFFSET = (START + BOOT_SECTORS) * 512

def main():
    smoke.IMAGE = ROOT / '.storage-test/log-test.img'
    raw = (ROOT / '.storage-test/root.img').read_bytes()
    volume = SystemVolume(raw[OFFSET:OFFSET + DATA_SECTORS * 512])
    volume.put('bin/logtest.elf', (ROOT / '.storage-test/log-probe.elf').read_bytes())
    volume.put('bin/readhash.elf', (ROOT / '.storage-test/read-hash-probe.elf').read_bytes())
    expected = 0xcbf29ce484222325
    for byte in volume.read('share/wall.raw'):
        expected = ((expected ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    smoke.IMAGE.write_bytes(raw[:OFFSET] + volume.data)
    q = smoke.Qemu()
    splash_seen = False
    try:
        for _ in range(150):
            time.sleep(.2)
            try:
                log = q.capture('handoff')
            except AssertionError as error:
                if 'Invalid addr' in str(error):
                    continue
                raise
            if '[ramfs] Root mounted' in log and '[storage] WYZARDATA mounted as root' not in log:
                ppm = (ROOT / '.storage-test/handoff.ppm').read_bytes()
                _, dims, _, pixels = ppm.split(b'\n', 3)
                w, _ = map(int, dims.split())
                assert pixels[(100*w+5)*3:(100*w+5)*3+3] == bytes((10,15,30)), 'Desktop visible before storage selection'
                splash_seen = True
            if '[wm] Shell task started inside WM frame.' in log:
                break
        else:
            raise AssertionError('Boot stalled: ' + log[-2500:])
        assert splash_seen, 'Did not observe the pre-storage splash'
        assert log.index('[wallpaper] Loaded disk asset into RAM.') < log.index('[clock] RTC OK:')
        elapsed = re.search(r'Load attempt milliseconds:\n(\d+)', log)[1]
        q.type('run /bin/logtest.elf\n', pause=3)
        q.type('run /bin/readhash.elf\n', pause=3)
        q.type('stat /bootlog.txt\n', pause=1)
        q.capture('live-log-stat')
    finally:
        q.close()
    with smoke.IMAGE.open('rb') as image:
        image.seek(OFFSET)
        updated = SystemVolume(image.read(DATA_SECTORS * 512))
    assert updated.read('logcheck.txt') == b'PASS', updated.read('logcheck.txt')
    assert updated.read('readhash.txt') == f'{expected:016x}'.encode(), updated.read('readhash.txt')
    print(f'QEMU passed: splash before storage, first desktop after wallpaper, 8.3 MB wallpaper {elapsed} ms, both live logs agree across stat/read/readdir, full wallpaper hash matches across 64 KiB reads')

if __name__ == '__main__':
    main()
