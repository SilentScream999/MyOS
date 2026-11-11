"""ext4 updater/migration preservation and failed-update atomicity (WSL/Linux)."""
from pathlib import Path
import hashlib
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from make_usb_image import (Fat16Image, START, BOOT_SECTORS, DATA_SECTORS,
                            write_image, validate_existing)
from system_volume import install_system
from ext4_volume import install_ext4, check
from migrate_fat16 import migrate


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.digest()


def main():
    with tempfile.TemporaryDirectory(prefix='wyzar-ext4-test-') as directory:
        work = Path(directory)
        root = work / 'build'; root.mkdir()
        for name in ('init.elf', 'counter.elf', 'win_test.elf'):
            (root / name).write_bytes(b'first-' + name.encode())
        (root / 'wallpaper.raw').write_bytes(b'original wallpaper')
        old = Fat16Image(DATA_SECTORS, 'WYZARDATA', START + BOOT_SECTORS)
        old.data = install_system(old.data, root)
        from system_volume import SystemVolume
        saved = SystemVolume(old.data)
        saved.put('home/saved.txt', b'personal document')
        saved.put('bin/user.elf', b'personal application')
        saved.put('etc/desktop.cfg', b'/home/custom.raw\n')
        saved.put('home/custom.raw', b'personal wallpaper')
        old.data = saved.data
        boot = Fat16Image(BOOT_SECTORS, 'WYZARBOOT', START)
        mbr = bytearray(512); mbr[510:512] = b'\x55\xaa'
        for i, (kind, start, count) in enumerate(((0xef, START, BOOT_SECTORS), (0x0e, START + BOOT_SECTORS, DATA_SECTORS))):
            struct.pack_into('<B3sB3sII', mbr, 446 + i * 16, 0, b'\xfe\xff\xff', kind, b'\xfe\xff\xff', start, count)
        original = work / 'old.img'
        write_image(original, boot, old, mbr)
        original_hash = digest(original)
        data = migrate(original, root)
        assert digest(original) == original_hash
        partition = work / 'partition.img'; partition.write_bytes(data)

        def read(path):
            return subprocess.check_output(['debugfs', '-R', 'cat ' + path, str(partition)], stderr=subprocess.DEVNULL)

        for path, content in (('/home/saved.txt', b'personal document'), ('/bin/user.elf', b'personal application'),
                              ('/etc/desktop.cfg', b'/home/custom.raw\n'), ('/home/custom.raw', b'personal wallpaper')):
            assert read(path) == content, path
        (root / 'init.elf').write_bytes(b'new shell')
        partition.write_bytes(install_ext4(data, root))
        assert read('/bin/init.elf') == b'new shell'
        assert read('/home/saved.txt') == b'personal document'
        assert read('/etc/desktop.cfg') == b'/home/custom.raw\n'
        assert read('/bin/user.elf') == b'personal application'
        assert read('/share/wall.raw') == b'original wallpaper'
        invalid = bytearray(partition.read_bytes())
        struct.pack_into('<I', invalid, 1024 + 96, 0x80000000)
        try:
            check(invalid)
            raise AssertionError('Unknown features accepted')
        except ValueError:
            pass
        # Existing image refresh must remain atomic even when a bundled input is missing.
        ext4_disk = work / 'ext4.img'
        mbr[466] = 0x83
        struct.pack_into('<I', mbr, 474, len(data) // 512)
        class Data:
            pass
        image_data = Data(); image_data.data = partition.read_bytes()
        write_image(ext4_disk, boot, image_data, mbr)
        with ext4_disk.open('rb') as source:
            assert validate_existing(source)[0] == 'ext4'
        old_hash = digest(ext4_disk)
        (root / 'counter.elf').unlink()
        try:
            write_image(ext4_disk, boot, None, mbr, update_boot=True, system_root=root)
            raise AssertionError('Missing update input accepted')
        except FileNotFoundError:
            pass
        assert digest(ext4_disk) == old_hash
        assert digest(original) == original_hash
    print('ext4 migration, refresh preservation, feature rejection and atomic publication passed')


if __name__ == '__main__':
    main()
