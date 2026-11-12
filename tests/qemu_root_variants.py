"""QEMU disk wallpaper selection, corrupt/missing asset, and recovery boots."""
from pathlib import Path
import struct
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from system_volume import SystemVolume
from make_usb_image import START, BOOT_SECTORS, DATA_SECTORS
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]
OFFSET = (START + BOOT_SECTORS) * 512


def boot(label, storage=True):
    q = smoke.Qemu(storage=storage)
    try:
        for _ in range(75):
            time.sleep(2)
            try:
                log = q.capture(label)
            except AssertionError as error:
                if 'Invalid addr' in str(error):
                    continue
                raise
            if '[init] Decoupled terminal boot.' in log:
                time.sleep(3)
                log = q.capture(label)
                assert '[wm] Shell task started inside WM frame.' in log, log[-2000:]
                return log
        raise AssertionError('Boot timeout: ' + log[-3000:])
    finally:
        q.close()


def main():
    source = (ROOT / '.storage-test/root.img').read_bytes()
    smoke.IMAGE = ROOT / '.storage-test/variants.img'
    smoke.BOOT_ISO = True
    volume = SystemVolume(source[OFFSET:OFFSET + DATA_SECTORS * 512])
    volume.put('home/blue.raw', b'WRAW' + struct.pack('<III', 1, 1, 0) + bytes((0xaa,0x55,0x22,255)))
    volume.put('etc/desktop.cfg', b'/home/blue.raw\n')
    def save():
        smoke.IMAGE.write_bytes(source[:OFFSET] + volume.data)
    save()
    log = boot('custom-wallpaper')
    assert '[storage] WYZARDATA mounted as root /.' in log
    assert '[wallpaper] Loaded disk asset into RAM.' in log
    # Sample an exposed desktop pixel; this color exists only in the disk asset.
    ppm = (ROOT / '.storage-test/custom-wallpaper.ppm').read_bytes()
    header, dimensions, maximum, pixels = ppm.split(b'\n', 3)
    w, h = map(int, dimensions.split())
    assert pixels[(100*w+5)*3:(100*w+5)*3+3] == bytes((0x22,0x55,0xaa))
    volume.put('home/blue.raw', b'WRAW' + bytes(12))
    save()
    assert '[wallpaper] Missing, unreadable or invalid asset; keeping fallback.' in boot('corrupt-wallpaper')
    volume.put('etc/desktop.cfg', b'/home/absent.raw\n')
    save()
    assert '[wallpaper] Missing, unreadable or invalid asset; keeping fallback.' in boot('missing-wallpaper')
    volume.put('etc/root.cfg', b'INVALID\n')
    save()
    assert '[storage] No unique valid system volume; RAM recovery mode.' in boot('invalid-root')
    assert '[storage] No unique valid system volume; RAM recovery mode.' in boot('no-disk', storage=False)
    print('QEMU variants passed: disk-only custom pixel, corrupt/missing wallpaper, invalid root, ISO without storage')


if __name__ == '__main__':
    main()
