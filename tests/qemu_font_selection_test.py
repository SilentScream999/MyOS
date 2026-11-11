"""Boot a disposable current USB image with a custom font and invalid selections.

Usage: python3 tests/qemu_font_selection_test.py BASE_IMAGE TTF_FONT
"""
from pathlib import Path
import sys
import time
import subprocess
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from make_usb_image import START, BOOT_SECTORS
import qemu_storage_smoke as smoke


def main():
    source = Path(sys.argv[1]).read_bytes()
    font = Path(sys.argv[2]).read_bytes()
    offset = (START + BOOT_SECTORS) * 512
    smoke.OUT.mkdir(exist_ok=True)
    smoke.IMAGE = smoke.OUT / 'font-selection.img'
    scratch = tempfile.TemporaryDirectory(prefix='wyzar-font-test-')
    work = Path(scratch.name)
    partition = work / 'partition.img'
    partition.write_bytes(source[offset:])
    helper = Path(__file__).resolve().parents[1] / '.storage-test/ext4-image-tool'
    subprocess.run(['make', '-s', '.storage-test/ext4-image-tool'], cwd=helper.parents[1], check=True)

    def put(path, content):
        asset = work / 'asset'
        asset.write_bytes(content)
        manifest = work / 'manifest'
        manifest.write_text(f'put /{path} {asset}\n')
        subprocess.run([str(helper), str(partition), str(manifest)], check=True)

    def boot(label, recovery=False):
        smoke.IMAGE.write_bytes(source[:offset] + partition.read_bytes())
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

    put('share/fonts/custom.ttf', font)
    put('etc/font.cfg', b'/share/fonts/custom.ttf\r\n')
    assert '[font] Loaded desktop font:' in boot('font-custom')
    put('share/fonts/custom.ttf', b'\x00\x01\x00\x00' + bytes(12))
    assert '[font] Invalid/unreadable font; using built-in Selawik.' in boot('font-corrupt')
    put('etc/font.cfg', b'/share/fonts/absent.ttf\n')
    assert '[font] Loaded desktop font:' not in boot('font-missing')
    put('etc/font.cfg', b'relative.ttf\n')
    assert '[font] Invalid /etc/font.cfg; using built-in Selawik.' in boot('font-invalid-config')
    assert '[font] Loaded desktop font:' in boot('font-recovery', recovery=True)
    scratch.cleanup()
    print('QEMU font selection passed: custom font, corrupt/missing asset, invalid config, recovery initrd')


if __name__ == '__main__':
    main()
