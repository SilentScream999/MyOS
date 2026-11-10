"""Migration, update preservation, malformed-volume and full-volume checks."""
from pathlib import Path
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from make_usb_image import Fat16Image, DATA_SECTORS, START, BOOT_SECTORS
from system_volume import SystemVolume, install_system


def main():
    old = Fat16Image(DATA_SECTORS, 'WYZARDATA', START + BOOT_SECTORS)
    old.add('SAVED.TXT', b'old saved document')
    old.add('bin/USER.ELF', b'user app')
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        for name in ('init.elf', 'counter.elf', 'win_test.elf'):
            (root / name).write_bytes(b'first-' + name.encode())
        (root / 'wallpaper.raw').write_bytes(b'initial asset')
        migrated = SystemVolume(install_system(old.data, root))
        assert migrated.read('saved.txt') == b'old saved document'
        assert migrated.read('bin/user.elf') == b'user app'
        assert migrated.read('bin/init.elf') == b'first-init.elf'
        assert migrated.read('etc/root.cfg') == b'WYZARROOT1\n'
        assert migrated.read('etc/font.cfg') == b'/share/fonts/selawk.ttf\n'
        fonts = root / 'assets/fonts/selawik'
        fonts.mkdir(parents=True)
        (fonts / 'selawk.ttf').write_bytes(b'ttf font')
        migrated = SystemVolume(install_system(migrated.data, root))
        assert migrated.read('share/fonts/selawk.ttf') == b'ttf font'
        migrated.put('etc/font.cfg', b'/share/fonts/mono.ttf\n')
        migrated.put('share/fonts/mono.ttf', b'personal font')
        (fonts / 'selawk.ttf').write_bytes(b'updated build font')
        migrated.put('etc/desktop.cfg', b'/home/custom.raw\n')
        migrated.put('home/custom.raw', b'personal wallpaper')
        migrated.put('share/wall.raw', b'edited default wallpaper')
        (root / 'init.elf').write_bytes(b'new shell' * 2000)
        (root / 'wallpaper.raw').write_bytes(b'new build wallpaper')
        updated = SystemVolume(install_system(migrated.data, root))
        assert updated.read('share/fonts/selawk.ttf') == b'updated build font'
        assert updated.read('bin/init.elf') == b'new shell' * 2000
        for path in ('saved.txt', 'bin/user.elf', 'etc/desktop.cfg', 'home/custom.raw', 'share/wall.raw',
                     'etc/font.cfg', 'share/fonts/mono.ttf'):
            assert updated.read(path) == migrated.read(path), path
        # Shrinking replacements release the previous cluster chain.
        (root / 'init.elf').write_bytes(b'small')
        small = SystemVolume(install_system(updated.data, root))
        assert small.read('bin/init.elf') == b'small'
        assert sum(small.fat(c) != 0 for c in range(2, small.count+2)) < sum(updated.fat(c) != 0 for c in range(2, updated.count+2))
        # Both FAT copies must agree; corrupt inputs must not be published.
        broken = bytearray(small.data)
        broken[520] ^= 1
        try:
            SystemVolume(broken)
            raise AssertionError('Accepted mismatched FATs')
        except ValueError:
            pass
        # No room for an upgraded program: fail rather than silently omit it.
        (root / 'init.elf').write_bytes(bytes(65 * 1024 * 1024))
        original = bytes(small.data)
        try:
            install_system(small.data, root)
            raise AssertionError('Accepted overfull update')
        except ValueError:
            pass
        assert bytes(small.data) == original
    print('System volume tests passed: migration, app upgrades, user/settings/wallpaper preservation, reclamation, corruption/full-volume rejection')


if __name__ == '__main__':
    main()
