"""ext4 creation, update preservation and failed-update atomicity (WSL/Linux)."""
from pathlib import Path
import hashlib
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from make_usb_image import (Fat16Image, START, BOOT_SECTORS,
                            write_image, validate_existing)
from ext4_volume import install_ext4, check
from types import SimpleNamespace


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
        partition = work / 'partition.img'
        partition.write_bytes(install_ext4(None, root))
        manifest = work / 'manifest'
        lines = []
        for i, (path, content) in enumerate((
            ('/home/saved.txt', b'personal document'),
            ('/bin/user.elf', b'personal application'),
            ('/etc/desktop.cfg', b'/home/custom.raw\n'),
            ('/home/custom.raw', b'personal wallpaper'),
            ('/etc/icons.cfg', b'personal-theme\n'),
            ('/share/icons/hicolor/32x32/apps/terminal.png', b'personal PNG replacement'),
        )):
            asset = work / f'personal-{i}'
            asset.write_bytes(content)
            lines.append(f'put {path} {asset}')
        manifest.write_text('\n'.join(lines) + '\n')
        subprocess.run([str(ROOT / '.storage-test/ext4-image-tool'), str(partition), str(manifest)], check=True)
        data = partition.read_bytes()
        boot = Fat16Image(BOOT_SECTORS, 'WYZARBOOT', START)
        mbr = bytearray(512); mbr[510:512] = b'\x55\xaa'
        for i, (kind, start, count) in enumerate(((0xef, START, BOOT_SECTORS), (0x83, START + BOOT_SECTORS, len(data) // 512))):
            struct.pack_into('<B3sB3sII', mbr, 446 + i * 16, 0, b'\xfe\xff\xff', kind, b'\xfe\xff\xff', start, count)

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
        assert read('/etc/icons.cfg') == b'personal-theme\n'
        assert read('/share/icons/hicolor/32x32/apps/terminal.png') == b'personal PNG replacement'
        assert read('/share/icons/hicolor/24x24/apps/application-default.png').startswith(b'\x89PNG')
        invalid = bytearray(partition.read_bytes())
        struct.pack_into('<I', invalid, 1024 + 96, 0x80000000)
        try:
            check(invalid)
            raise AssertionError('Unknown features accepted')
        except ValueError:
            pass
        # Existing image refresh must remain atomic even when a bundled input is missing.
        ext4_disk = work / 'ext4.img'
        image_data = SimpleNamespace(data=partition.read_bytes())
        write_image(ext4_disk, boot, image_data, mbr)
        with ext4_disk.open('rb') as source:
            assert validate_existing(source) == len(data) // 512
        (root / 'init.elf').write_bytes(b'latest shell')
        write_image(ext4_disk, boot, None, mbr, update_boot=True, system_root=root)
        with ext4_disk.open('rb') as source:
            source.seek((START + BOOT_SECTORS) * 512)
            partition.write_bytes(source.read())
        assert read('/bin/init.elf') == b'latest shell'
        assert read('/home/saved.txt') == b'personal document'
        assert read('/etc/desktop.cfg') == b'/home/custom.raw\n'
        assert read('/bin/user.elf') == b'personal application'
        old_hash = digest(ext4_disk)
        (root / 'counter.elf').unlink()
        try:
            write_image(ext4_disk, boot, None, mbr, update_boot=True, system_root=root)
            raise AssertionError('Missing update input accepted')
        except FileNotFoundError:
            pass
        assert digest(ext4_disk) == old_hash
        # A FAT system partition must be rejected without changing the image.
        with ext4_disk.open('r+b') as disk:
            disk.seek(466)
            disk.write(b'\x0e')
        rejected_hash = digest(ext4_disk)
        try:
            write_image(ext4_disk, boot, None, mbr, update_boot=True, system_root=root)
            raise AssertionError('Legacy system partition accepted')
        except ValueError:
            pass
        assert digest(ext4_disk) == rejected_hash
        # Explicit fresh replacement can reset an otherwise rejected layout.
        write_image(ext4_disk, boot, image_data, mbr, fresh=True)
        with ext4_disk.open('rb') as source:
            assert validate_existing(source) == len(data) // 512
    print('ext4 creation, refresh preservation, legacy/feature rejection and atomic publication passed')


if __name__ == '__main__':
    main()
