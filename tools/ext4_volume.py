"""Build/update a private ext4 partition file. Requires Linux e2fsprogs and cc.

Never opens a physical device. The caller owns locking and atomic publication.
"""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

EXT4_SECTORS = 524288  # 256 MiB, independently expandable in future layouts.
MIN_SECTORS = 131072
MAX_SECTORS = ((1 << 32) - 1 - 264192) & ~7
FEATURES = 'none,has_journal,extent,filetype,dir_index,sparse_super,large_file,extra_isize'
ROOT = Path(__file__).resolve().parent.parent


def run(*args):
    subprocess.run([str(a) for a in args], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def check(data):
    check_header(data[1024:2048], len(data))


def check_file(path):
    with Path(path).open('rb') as source:
        source.seek(1024)
        check_header(source.read(1024), Path(path).stat().st_size)


def check_header(sb, length):
    if length % 4096 or not MIN_SECTORS * 512 <= length <= MAX_SECTORS * 512 or len(sb) != 1024:
        raise ValueError('Unsupported ext4 partition size')
    if sb[56:58] != b'\x53\xef' or sb[120:136] != b'WYZARDATA'.ljust(16, b'\0'):
        raise ValueError('Unrecognized ext4 system volume')
    compat, incompat, ro = struct.unpack_from('<III', sb, 92)
    if (not compat & 4 or compat & ~0x2c or incompat & ~0x46 or ro & ~0x43 or
            struct.unpack_from('<I', sb, 24)[0] != 2 or
            struct.unpack_from('<I', sb, 4)[0] > length // 4096 or
            struct.unpack_from('<I', sb, 228)[0] or struct.unpack_from('<I', sb, 232)[0]):
        raise ValueError('Unsupported ext4 feature profile')


def install_ext4(data, root):
    """Byte-buffer compatibility wrapper for small test fixtures."""
    with tempfile.TemporaryDirectory(prefix='wyzar-ext4-buffer-') as temporary:
        volume = Path(temporary) / 'volume.img'
        if data is not None:
            volume.write_bytes(data)
        install_ext4_file(volume, root, create=data is None)
        return volume.read_bytes()


def install_ext4_file(volume, root, create=False, sectors=EXT4_SECTORS):
    """Edit only a caller-owned private partition file; never the live image."""
    volume = Path(volume)
    for tool in ('mke2fs', 'e2fsck', 'make'):
        if not shutil.which(tool):
            raise RuntimeError('ext4 image creation requires WSL/Linux with e2fsprogs and make')
    if not create:
        check_file(volume)
    elif sectors % 8 or not MIN_SECTORS <= sectors <= MAX_SECTORS:
        raise ValueError('Unsupported ext4 partition size')
    # Dependency tracking rebuilds the helper when its source/library changes.
    subprocess.run(['make', '-s', '.storage-test/ext4-image-tool'], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix='wyzar-ext4-') as temporary:
        work = Path(temporary)
        if create:
            with volume.open('wb') as f:
                f.truncate(sectors * 512)
            run('mke2fs', '-q', '-t', 'ext4', '-F', '-b', '4096', '-I', '256', '-m', '0',
                '-L', 'WYZARDATA', '-O', FEATURES,
                '-E', 'lazy_itable_init=0,lazy_journal_init=0',
                volume)
        # Replay/check only our private copy. Never repair the original image.
        result = subprocess.run(['e2fsck', '-p', str(volume)], capture_output=True)
        if result.returncode not in (0, 1):
            raise ValueError('ext4 check/recovery failed: ' + result.stdout.decode(errors='replace'))
        lines = []
        for directory in ('bin', 'apps', 'etc', 'home', 'share', 'share/fonts', 'share/icons', 'share/samples', 'home/Applications', 'tmp', 'run', 'dev', 'mnt'):
            lines.append('mkdir /' + directory)

        def put(target, content, default=False):
            source = work / ('asset-' + str(len(lines)))
            source.write_bytes(content)
            lines.append(f'{"seed" if default else "put"} /{target} {source}')

        for program in ('init.elf', 'counter.elf', 'win_test.elf'):
            put('bin/' + program, (root / program).read_bytes())
        # Sample packages: run them in place or `wiz install` them (docs/APP_PACKAGES.md).
        for package in ('counter.wiz', 'win_test.wiz', 'bundle_demo.wiz'):
            source = root / 'build' / 'packages' / package
            if source.exists():
                put('share/samples/' + package, source.read_bytes())
        put('etc/root.cfg', b'WYZARROOT1\n')
        put('etc/desktop.cfg', b'/share/wall.raw\n', True)
        put('etc/font.cfg', b'/share/fonts/selawk.ttf\n', True)
        put('etc/icons.cfg', b'hicolor\n', True)
        put('bootlog.txt', b'', True)
        put('run/boot.log', b'', True)
        for target, source, default in (
            ('share/wall.raw', 'wallpaper.raw', True),
            ('share/fonts/selawik.txt', 'assets/fonts/selawik/LICENSE.txt', False),
            ('share/fonts/selawk.ttf', 'assets/fonts/selawik/selawk.ttf', False),
        ):
            if (root / source).exists():
                put(target, (root / source).read_bytes(), default)
        # Ship the dependency license alongside installed OS files.
        put('share/lwext4-license.txt', (ROOT / 'third_party/lwext4/LICENSE').read_bytes())
        put('share/stb-image-license.txt', (ROOT / 'third_party/stb/LICENSE').read_bytes())
        # Seed replaceable default artwork without overwriting installed themes.
        for asset in sorted((ROOT / 'assets/icons/hicolor').rglob('*.png')):
            relative = asset.relative_to(ROOT / 'assets/icons').as_posix()
            parent = 'share/icons/' + relative.rpartition('/')[0]
            parts = parent.split('/')
            for index in range(3, len(parts)+1):
                lines.append('mkdir /' + '/'.join(parts[:index]))
            put('share/icons/' + relative, asset.read_bytes(), True)
        manifest = work / 'manifest'
        manifest.write_text('\n'.join(lines) + '\n')
        run(ROOT / '.storage-test/ext4-image-tool', volume, manifest)
        run('e2fsck', '-fn', volume)
        check_file(volume)
