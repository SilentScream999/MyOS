"""Boot a disposable current USB image with a custom font and invalid selections.

Usage: python3 tests/qemu_font_selection_test.py BASE_IMAGE TTF_FONT
"""
from pathlib import Path
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from system_volume import SystemVolume
from make_usb_image import START, BOOT_SECTORS, DATA_SECTORS
import qemu_storage_smoke as smoke


def main():
    source = Path(sys.argv[1]).read_bytes()
    font = Path(sys.argv[2]).read_bytes()
    offset = (START + BOOT_SECTORS) * 512
    volume = SystemVolume(source[offset:offset + DATA_SECTORS * 512])
    smoke.OUT.mkdir(exist_ok=True)
    smoke.IMAGE = smoke.OUT / 'font-selection.img'

    def boot(label, recovery=False):
        smoke.IMAGE.write_bytes(source[:offset] + volume.data)
        smoke.BOOT_ISO = recovery
        q = smoke.Qemu(storage=not recovery)
        log = ''
        try:
            for _ in range(45):
                time.sleep(1)
                try:
                    log = q.capture(label)
                except AssertionError:
                    continue
                if '[wm] Shell task started inside WM frame.' in log:
                    return log
            raise AssertionError('Boot timeout: ' + log[-3000:])
        finally:
            q.close()

    volume.put('share/fonts/custom.ttf', font)
    volume.put('etc/font.cfg', b'/share/fonts/custom.ttf\r\n')
    assert '[font] Loaded desktop font:' in boot('font-custom')
    volume.put('share/fonts/custom.ttf', b'\x00\x01\x00\x00' + bytes(12))
    assert '[font] Invalid/unreadable font; using built-in Selawik.' in boot('font-corrupt')
    volume.put('etc/font.cfg', b'/share/fonts/absent.ttf\n')
    assert '[font] Loaded desktop font:' not in boot('font-missing')
    volume.put('etc/font.cfg', b'relative.ttf\n')
    assert '[font] Invalid /etc/font.cfg; using built-in Selawik.' in boot('font-invalid-config')
    assert '[font] Loaded desktop font:' in boot('font-recovery', recovery=True)
    print('QEMU font selection passed: custom font, corrupt/missing asset, invalid config, recovery initrd')


if __name__ == '__main__':
    main()
