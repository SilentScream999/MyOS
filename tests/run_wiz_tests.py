#!/usr/bin/env python3
"""Tests for the .wiz package format: the Python packer, then the C reader.

1. Exercises tools/wiz_pack.py (round trips, determinism, rejection of bad input).
2. Generates fixture packages, including deliberately malicious ones.
3. Builds tests/wiz_test.cpp with sanitizers and runs it against the fixtures,
   which checks the independent C implementation (src/core/wiz.h) agrees with
   the packer and survives corruption of every byte of every package.

Set CXX to choose a compiler (default: clang++, else g++).
"""
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import wiz_pack as wp  # noqa: E402

OUT = ROOT / '.storage-test/wiz-fixtures'


def minimal_elf(extra=b'', phdr_type=1, e_type=2, machine=0x3E, ei_class=2):
    """Smallest ELF that passes the packer's checks: header + one program header."""
    header = bytearray(64)
    header[0:4] = b'\x7fELF'
    header[4], header[5], header[6] = ei_class, 1, 1
    struct.pack_into('<HHIQQQIHHHHHH', header, 16, e_type, machine, 1, 0x400078, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    phdr = struct.pack('<IIQQQQQQ', phdr_type, 5, 0, 0x400000, 0x400000, 0x80, 0x80, 0x1000)
    return bytes(header) + phdr + b'\xf4' * 8 + extra


def expect_error(label, function):
    try:
        function()
    except wp.PackError:
        return
    raise AssertionError('expected PackError: ' + label)


def typed_header(name, size, type_flag):
    header = bytearray(wp.tar_header(name, size))
    header[156] = ord(type_flag)
    header[148:156] = b' ' * 8
    header[148:156] = b'%06o\0 ' % sum(header)
    return bytes(header)


def raw_tar(items):
    """items: (name, data, type_flag). No safety checks: for building bad packages."""
    out = bytearray()
    for name, data, flag in items:
        out += typed_header(name, len(data), flag) + data + b'\0' * (-len(data) % 512)
    return bytes(out) + b'\0' * 1024


GOOD_MANIFEST = (b'id=counter\nname=Counter\nversion=1.2.3\nabi=1\n'
                 b'description=Counts things.\nfuture-key=ignored\n')


def python_tests(tmp, alpha, blue):
    elf = minimal_elf()
    manifest = tmp / 'app.manifest'
    manifest.write_bytes(GOOD_MANIFEST)
    icons = tmp / 'icons'
    for size, data in ((16, alpha), (32, blue)):
        (icons / ('%dx%d' % (size, size)) / 'apps').mkdir(parents=True, exist_ok=True)
        (icons / ('%dx%d' % (size, size)) / 'apps' / 'counter.png').write_bytes(data)
    (tmp / 'app.elf').write_bytes(elf)

    def run(*extra, elf_name='app.elf', manifest_path=manifest, output='out.wiz'):
        argv = ['pack', '--elf', str(tmp / elf_name), '--manifest', str(manifest_path),
                '-o', str(tmp / output)] + list(extra)
        if wp.main(argv) != 0:
            raise wp.PackError('wiz_pack exited with an error')

    run('--icons', str(icons))
    first = (tmp / 'out.wiz').read_bytes()
    run('--icons', str(icons), output='again.wiz')
    assert first == (tmp / 'again.wiz').read_bytes(), 'packing must be deterministic'
    package = wp.load(tmp / 'out.wiz')
    values, entries = package.verify()
    assert values['id'] == 'counter' and values['version'] == '1.2.3'
    assert [n for n, _ in entries] == ['app.manifest', 'icons/16.png', 'icons/32.png']
    assert first[:len(elf)] == elf, 'ELF image must be unchanged inside the package'
    if shutil.which('tar'):
        payload = first[package.payload_offset:package.payload_offset + package.payload_size]
        (tmp / 'payload.tar').write_bytes(payload)
        listing = subprocess.run(['tar', '-tf', str(tmp / 'payload.tar')], capture_output=True, text=True, check=True)
        assert listing.stdout.split() == ['app.manifest', 'icons/16.png', 'icons/32.png']
    wp.main(['extract', str(tmp / 'out.wiz'), str(tmp / 'x')])
    assert (tmp / 'x/icons/32.png').read_bytes() == blue

    data_dir = tmp / 'data'
    (data_dir / 'sub').mkdir(parents=True)
    (data_dir / 'sub/level.txt').write_text('hello')
    run('--data', str(data_dir), output='data.wiz')
    _, entries = wp.load(tmp / 'data.wiz').verify()
    assert ('data/sub/level.txt', b'hello') in entries
    (data_dir / 'bad name.txt').write_text('x')
    expect_error('space in data file name', lambda: run('--data', str(data_dir), output='bad.wiz'))

    # Input validation.
    (tmp / 'text.elf').write_bytes(b'not an elf at all' * 10)
    expect_error('non-ELF', lambda: run(elf_name='text.elf'))
    (tmp / 'dyn.elf').write_bytes(minimal_elf(phdr_type=3))
    expect_error('PT_INTERP', lambda: run(elf_name='dyn.elf'))
    (tmp / 'et_dyn.elf').write_bytes(minimal_elf(e_type=3))
    expect_error('ET_DYN', lambda: run(elf_name='et_dyn.elf'))
    (tmp / 'arm.elf').write_bytes(minimal_elf(machine=0xB7))
    expect_error('wrong machine', lambda: run(elf_name='arm.elf'))
    (tmp / 'elf32.elf').write_bytes(minimal_elf(ei_class=1))
    expect_error('32-bit', lambda: run(elf_name='elf32.elf'))
    for name, text in (('missing-id', b'name=A\nversion=1\nabi=1\n'),
                       ('bad-id', b'id=../x\nname=A\nversion=1\nabi=1\n'),
                       ('dot-id', b'id=..\nname=A\nversion=1\nabi=1\n'),
                       ('missing-version', b'id=a\nname=A\nabi=1\n'),
                       ('abi-zero', b'id=a\nname=A\nversion=1\nabi=0\n'),
                       ('abi-future', b'id=a\nname=A\nversion=1\nabi=2\n'),
                       ('abi-text', b'id=a\nname=A\nversion=1\nabi=x\n'),
                       ('duplicate', b'id=a\nid=b\nname=A\nversion=1\nabi=1\n'),
                       ('empty-value', b'id=a\nname=\nversion=1\nabi=1\n'),
                       ('no-equals', b'id=a\nname=A\nversion=1\nabi=1\njunk\n'),
                       ('control-char', b'id=a\nname=A\x01\nversion=1\nabi=1\n'),
                       ('non-ascii', 'id=a\nname=\u00e9\nversion=1\nabi=1\n'.encode()),
                       ('long-name', b'id=a\nname=' + b'n' * 64 + b'\nversion=1\nabi=1\n'),
                       ('bad-icon-name', b'id=a\nname=A\nversion=1\nabi=1\nicon=../up\n')):
        path = tmp / (name + '.manifest')
        path.write_bytes(text)
        expect_error(name, lambda path=path: run(manifest_path=path))
    (tmp / 'ok.manifest').write_bytes(b'# c\r\nid = a.b-c_1 \r\nname = A B\r\nversion=2\r\nabi=1\r\n')
    run(manifest_path=tmp / 'ok.manifest', output='crlf.wiz')
    (tmp / 'text.png').write_bytes(b'not a png')
    (icons / '48x48' / 'apps').mkdir(parents=True)
    (icons / '48x48' / 'apps' / 'counter.png').write_bytes(b'not a png')
    expect_error('non-PNG icon', lambda: run('--icons', str(icons), output='badicon.wiz'))
    shutil.rmtree(icons / '48x48')

    # The reader rejects damaged packages.
    damaged = bytearray(first)
    damaged[-1] ^= 1
    expect_error('footer crc', lambda: wp.Package(bytes(damaged)))
    damaged = bytearray(first)
    damaged[len(elf) + 600] ^= 1
    expect_error('payload crc', lambda: wp.Package(bytes(damaged)).verify())
    damaged = bytearray(first)
    damaged[18] ^= 1
    expect_error('ELF crc', lambda: wp.Package(bytes(damaged)).verify())
    legacy = wp.assemble(elf, [('app.manifest', GOOD_MANIFEST)], version=1)
    assert wp.Package(legacy).verify()[0]['id'] == 'counter'
    shadow = wp.assemble(elf, [('app.manifest', GOOD_MANIFEST),
                             ('app.manifest', b'id=evil\nname=Evil\nversion=1\nabi=1\n'),
                             ('data/value', b'first'), ('data/value', b'last')])
    assert wp.Package(shadow).verify()[0]['id'] == 'counter'
    (tmp / 'shadow.wiz').write_bytes(shadow)
    wp.main(['extract', str(tmp / 'shadow.wiz'), str(tmp / 'shadow')])
    assert (tmp / 'shadow/app.manifest').read_bytes() == GOOD_MANIFEST
    assert (tmp / 'shadow/data/value').read_bytes() == b'first'
    expect_error('plain elf', lambda: wp.Package(elf))
    expect_error('truncated', lambda: wp.Package(first[:-5]))
    print('python packer tests: ok')



def make_package(elf, manifest_text, icons=()):
    entries = [('app.manifest', manifest_text.encode())] + [('icons/%d.png' % n, d) for n, d in icons]
    return wp.assemble(elf, entries)


def shell_tests(tmp, alpha, blue):
    """Run the real `wiz` shell command (user/wiz_cmd.h) against a sandbox directory."""
    compiler = os.environ.get('CC') or shutil.which('clang') or shutil.which('gcc')
    binary = ROOT / '.storage-test/wiz-cmd-host'
    binary.parent.mkdir(exist_ok=True)
    subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-no-pie', '-fsanitize=address,undefined', '-Wall', '-Wextra',
                    '-Wno-unused-function', '-I.', 'tests/wiz_cmd_host.c', '-o', str(binary)], cwd=ROOT, check=True)
    root = tmp / 'sandbox'
    for d in ('apps', 'src', 'mnt/usb', 'home/.local/share/counter'):
        (root / d).mkdir(parents=True)
    elf = minimal_elf()

    def manifest(id_, name, version):
        return 'id=%s\nname=%s\nversion=%s\nabi=1\ndescription=Test app.\n' % (id_, name, version)

    v10 = make_package(elf, manifest('counter', 'Counter', '1.0.0'), [(16, alpha), (32, blue)])
    v12 = make_package(elf, manifest('counter', 'Counter', '1.2.0'), [(16, alpha)])
    v09 = make_package(elf, manifest('counter', 'Counter', '0.9.0'))
    other = make_package(elf, manifest('other-app', 'Other App', '2.0'), [(16, blue)])
    broken = bytearray(make_package(elf, manifest('broken', 'Broken', '1.0')))
    broken[len(elf) + 700] ^= 1
    (root / 'src/counter-1.0.wiz').write_bytes(v10)
    (root / 'src/counter-1.2.wiz').write_bytes(v12)
    (root / 'src/counter-0.9.wiz').write_bytes(v09)
    (root / 'src/broken.wiz').write_bytes(bytes(broken))
    (root / 'src/plain.wiz').write_bytes(elf)
    (root / 'mnt/usb/other.wiz').write_bytes(other)
    (root / 'home/.local/share/counter/save.dat').write_text('progress')

    def wiz(cwd, *commands, env=None):
        run = subprocess.run([str(binary), str(root), cwd] + list(commands), capture_output=True, text=True, env=env)
        assert run.returncode == 0, run.stderr + run.stdout
        return run.stdout

    out = wiz('/', 'list')
    assert 'No installed or pinned apps' in out, out
    # Install, then confirm the on-disk result exactly.
    out = wiz('/', 'install /src/counter-1.0.wiz')
    assert 'installed Counter 1.0.0' in out and 'Start menu' in out, out
    assert (root / 'apps/counter/counter.wiz').read_bytes() == v10
    registration = (root / 'apps/counter/app.manifest').read_text()
    assert 'exec=/apps/counter/counter.wiz' in registration and 'kind=installed' in registration, registration
    assert not (root / 'apps/counter/install.tmp').exists(), 'temporary file left behind'
    assert wp.parse_manifest(registration.encode())['exec'] == '/apps/counter/counter.wiz'
    out = wiz('/', 'install /src/counter-1.2.wiz', env=dict(os.environ, WIZ_TEST_EXCHANGE_FAIL='1'))
    assert 'cannot publish' in out, out
    assert (root / 'apps/counter/counter.wiz').read_bytes() == v10
    assert (root / 'apps/counter/install.tmp').read_bytes() == v12
    assert (root / 'apps/counter/app.manifest').read_text() == registration
    # Both the former failure boundary and maximum supported ID remain manageable.
    for length in (57, 64):
        ident = 'a' * length
        long_pkg = make_package(elf, manifest(ident, 'Long ID', '1'))
        (root / 'src/long.wiz').write_bytes(long_pkg)
        out = wiz('/', 'install /src/long.wiz', 'list', 'info ' + ident, 'remove ' + ident)
        assert 'installed Long ID' in out and 'Long ID 1' in out and 'removed ' + ident in out, out
        assert not (root / 'apps' / ident).exists()
    out = wiz('/', 'list')
    assert 'counter' in out and 'installed' in out and '1.0.0' in out and 'UNAVAILABLE' not in out, out
    # Upgrade keeps the user's own customizations in the registration.
    with open(root / 'apps/counter/app.manifest', 'a') as handle:
        handle.write('name=My Counter\n')
    out = wiz('/', 'install /src/counter-1.2.wiz')
    assert 'updated Counter 1.2.0 (was 1.0.0)' in out, out
    assert (root / 'apps/counter/counter.wiz').read_bytes() == v12
    assert 'name=My Counter' in (root / 'apps/counter/app.manifest').read_text()
    # Downgrades need -f.
    out = wiz('/', 'install /src/counter-0.9.wiz')
    assert 'newer version (1.2.0) is installed' in out, out
    assert (root / 'apps/counter/counter.wiz').read_bytes() == v12
    out = wiz('/', 'install -f /src/counter-0.9.wiz')
    assert 'updated Counter 0.9.0' in out, out
    # Relative paths resolve against the shell's directory, including "..".
    out = wiz('/src', 'install counter-1.0.wiz')
    assert 'updated Counter 1.0.0 (was 0.9.0)' in out, out
    out = wiz('/mnt/usb', 'info ../../src/counter-1.2.wiz')
    assert 'Counter 1.2.0' in out and 'checksum ok' in out and 'icons/16.png' in out, out
    # Info by id follows the registration.
    out = wiz('/', 'info counter')
    assert 'Counter 1.0.0' in out and '/apps/counter/counter.wiz' in out, out
    # Installing the installed copy onto itself is refused and harmless.
    out = wiz('/', 'install /apps/counter/counter.wiz')
    assert 'already installed from this file' in out, out
    assert (root / 'apps/counter/counter.wiz').read_bytes() == v10
    # Bad inputs never leave anything behind.
    out = wiz('/', 'install /src/broken.wiz')
    assert 'corrupt' in out and not (root / 'apps/broken').exists(), out
    out = wiz('/', 'install /src/plain.wiz')
    assert 'not a .wiz package' in out and not (root / 'apps/plain').exists(), out
    out = wiz('/', 'install /src/missing.wiz')
    assert 'no such file' in out, out
    out = wiz('/', 'info nosuchapp')
    assert "no installed or pinned app named 'nosuchapp'" in out, out
    # Pinning records the location without copying.
    out = wiz('/', 'pin /mnt/usb/other.wiz')
    assert 'pinned Other App 2.0' in out, out
    pinned = wp.parse_manifest((root / 'apps/other-app/app.manifest').read_bytes())
    assert pinned['exec'] == '/mnt/usb/other.wiz' and pinned['kind'] == 'pinned' and pinned['name'] == 'Other App'
    assert sorted(p.name for p in (root / 'apps/other-app').iterdir()) == ['app.manifest']
    out = wiz('/', 'list')
    assert 'other-app' in out and 'pinned' in out and '2.0' in out and 'UNAVAILABLE' not in out, out
    (root / 'mnt/usb/other.wiz').rename(root / 'mnt/usb/other.gone')           # unplug the drive
    out = wiz('/', 'list')
    assert 'other-app' in out and 'UNAVAILABLE' in out, out
    (root / 'mnt/usb/other.gone').rename(root / 'mnt/usb/other.wiz')
    # An installed app cannot be pinned over; hand-made registrations are protected.
    (root / 'mnt/usb/counter.wiz').write_bytes(v12)
    out = wiz('/', 'pin /mnt/usb/counter.wiz')
    assert "is installed; run 'wiz remove counter" in out, out
    (root / 'apps/manual').mkdir()
    (root / 'apps/manual/app.manifest').write_text('name=Manual\nexec=/bin/x.elf\n')
    out = wiz('/', 'remove manual')
    assert 'registered by hand' in out and (root / 'apps/manual/app.manifest').exists(), out
    out = wiz('/', 'list')
    assert 'manual' in out, out
    # Remove keeps data unless asked; unpin leaves the file alone.
    out = wiz('/', 'remove counter')
    assert 'removed counter' in out and 'kept its data' in out, out
    assert not (root / 'apps/counter').exists() and (root / 'home/.local/share/counter/save.dat').exists()
    out = wiz('/', 'install /src/counter-1.0.wiz', 'install /src/counter-1.0.wiz', 'remove --purge counter')
    assert 'reinstalled Counter 1.0.0' in out, out
    assert 'deleted its data' in out, out
    assert not (root / 'apps/counter').exists() and not (root / 'home/.local/share/counter').exists()
    out = wiz('/', 'remove other-app')
    assert 'unpinned other-app' in out and (root / 'mnt/usb/other.wiz').exists(), out
    assert not (root / 'apps/other-app').exists()
    out = wiz('/', 'remove ../etc')
    assert 'not a valid app id' in out, out
    out = wiz('/', 'bogus')
    assert 'Usage: wiz' in out, out
    print('wiz shell command tests: ok')



def wizlib_tests(tmp):
    """user/wizlib.h (what a running app uses) against faked syscalls."""
    compiler = os.environ.get('CC') or shutil.which('clang') or shutil.which('gcc')
    binary = ROOT / '.storage-test/wizlib-host'
    binary.parent.mkdir(exist_ok=True)
    subprocess.run([compiler, '-std=gnu11', '-O1', '-g', '-no-pie', '-fsanitize=address,undefined', '-Wall', '-Wextra',
                    '-Wno-unused-function', '-I.', 'tests/wizlib_host.c', '-o', str(binary)], cwd=ROOT, check=True)
    root = tmp / 'root'
    (root / 'mnt/usb').mkdir(parents=True)
    elf = minimal_elf()
    data = [('app.manifest', b'id=demo-app\nname=Demo App\nversion=4.2\nabi=1\n'),
            ('data/greeting.txt', b'Hello, bundle!'),
            ('data/big.bin', bytes((i * 7) & 255 for i in range(5000)))]
    (root / 'mnt/usb/demo.wiz').write_bytes(wp.assemble(elf, data))
    (root / 'mnt/usb/plain.wiz').write_bytes(elf)
    (root / 'mnt/usb/future.wiz').write_bytes(wp.assemble(elf, [('app.manifest', b'id=f\nname=F\nversion=1\nabi=2\n')]))
    run = subprocess.run([str(binary), str(root), '/mnt/usb/demo.wiz', '/mnt/usb/plain.wiz', '/mnt/usb/future.wiz'],
                         capture_output=True, text=True)
    assert run.returncode == 0 and 'wizlib host test passed' in run.stdout, run.stdout + run.stderr
    assert (root / 'home/.local/share/demo-app/score.txt').read_bytes() == b'98765'
    assert not (root / 'home/.local/escape.txt').exists() and not (root / 'escape.txt').exists()
    print('wizlib tests: ok')


def write_fixtures(alpha, blue):
    OUT.mkdir(parents=True, exist_ok=True)
    elf = minimal_elf()
    (OUT / 'good.elf').write_bytes(elf)
    (OUT / 'plain.elf').write_bytes(elf)
    entries = [('app.manifest', GOOD_MANIFEST), ('icons/16.png', alpha), ('icons/32.png', blue),
               ('data/readme.txt', b'portable data')]
    (OUT / 'good.wiz').write_bytes(wp.assemble(elf, entries))
    (OUT / 'legacy.wiz').write_bytes(wp.assemble(elf, entries, version=1))
    (OUT / 'alpha.png').write_bytes(alpha)
    (OUT / 'blue.png').write_bytes(blue)

    def variant(name, items, version=wp.FORMAT_VERSION):
        (OUT / name).write_bytes(wp.assemble(elf, None, version=version, payload=raw_tar(items)))

    good_manifest = ('app.manifest', GOOD_MANIFEST, '0')
    variant('no-manifest.wiz', [('icons/16.png', alpha, '0')])
    variant('abi2.wiz', [('app.manifest', b'id=x\nname=X\nversion=1\nabi=2\n', '0')])
    variant('future-format.wiz', [good_manifest], version=wp.FORMAT_VERSION + 1)
    variant('dotdot.wiz', [good_manifest, ('../evil', b'x', '0')])
    variant('absolute.wiz', [good_manifest, ('/etc/passwd', b'x', '0')])
    variant('symlink.wiz', [good_manifest, ('icons/16.png', b'', '2')])
    variant('pax.wiz', [('pax', b'x', 'x'), good_manifest])
    variant('trailing-slash.wiz', [good_manifest, ('data/a/', b'x', '0')])
    variant('bad-id.wiz', [('app.manifest', b'id=../../x\nname=X\nversion=1\nabi=1\n', '0')])
    # Two manifests: the first one wins, consistently, everywhere.
    variant('shadow.wiz', [good_manifest, ('app.manifest', b'id=evil\nname=Evil\nversion=9\nabi=1\n', '0')])
    # An entry whose declared size runs far past the payload.
    header = bytearray(wp.tar_header('app.manifest', 10))
    header[124:136] = b'77777777777\0'
    header[148:156] = b' ' * 8
    header[148:156] = b'%06o\0 ' % sum(header)
    (OUT / 'huge-size.wiz').write_bytes(
        wp.assemble(elf, None, payload=bytes(header) + b'\0' * 1024))
    # No end-of-archive marker.
    (OUT / 'no-end.wiz').write_bytes(
        wp.assemble(elf, None, payload=wp.tar_header('app.manifest', len(GOOD_MANIFEST)) +
                    GOOD_MANIFEST.ljust(512, b'\0')))
    other = [('app.manifest', b'id=other-app\nname=Other App\nversion=2.0\nabi=1\n'), ('icons/16.png', blue)]
    (OUT / 'other.wiz').write_bytes(wp.assemble(elf, other))
    # A footer that lies about where the payload is.
    good = (OUT / 'good.wiz').read_bytes()
    body = bytearray(good[-wp.FOOTER_SIZE:-4])
    struct.pack_into('<Q', body, 16, len(elf) + 512)
    (OUT / 'lying-footer.wiz').write_bytes(good[:-wp.FOOTER_SIZE] + bytes(body) + struct.pack('<I', zlib.crc32(bytes(body))))
    print('fixtures: %d files in %s' % (len(list(OUT.iterdir())), OUT))


def compile_and_run():
    compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        raise SystemExit('no C++ compiler found (set CXX)')
    flags = [compiler, '-std=c++20', '-O1', '-g', '-no-pie', '-fsanitize=address,undefined',
             '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Wno-misleading-indentation']
    binary = ROOT / '.storage-test/wiz-test'
    subprocess.run(flags + ['tests/wiz_test.cpp', 'src/gui/image.cpp', '-o', str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary), str(OUT)], cwd=ROOT, check=True)


def main():
    spec = importlib.util.spec_from_file_location('run_image_tests', ROOT / 'tests/run_image_tests.py')
    images = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(images)
    images.fixtures()
    alpha = (images.OUT / 'alpha.png').read_bytes()
    blue = (images.OUT / 'blue.png').read_bytes()
    with tempfile.TemporaryDirectory() as tmp:
        python_tests(Path(tmp), alpha, blue)
    with tempfile.TemporaryDirectory() as tmp:
        shell_tests(Path(tmp), alpha, blue)
    with tempfile.TemporaryDirectory() as tmp:
        wizlib_tests(Path(tmp))
    write_fixtures(alpha, blue)
    compile_and_run()


if __name__ == '__main__':
    main()
