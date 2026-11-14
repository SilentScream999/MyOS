#!/usr/bin/env python3
"""Build and inspect WyzarOS .wiz application packages.

A .wiz is a static x86-64 ELF followed by a ustar payload and a 40-byte footer
(see src/core/wiz.h for the authoritative format description):

    [ ELF image ][ tar: app.manifest, icons/<N>.png, data/... ][ footer ]

Usage:
    wiz_pack.py pack --elf counter.elf --manifest assets/apps/counter/app.manifest \\
                     --icons assets/icons/hicolor --icon-name counter -o build/packages/counter.wiz
    wiz_pack.py info    build/packages/counter.wiz      # manifest, icons, entries
    wiz_pack.py verify  build/packages/counter.wiz      # full structural + checksum validation
    wiz_pack.py extract build/packages/counter.wiz DIR  # unpack payload into DIR

Packing is deterministic: the same inputs always give byte-identical output.
"""
import argparse
import re
import struct
import sys
import zlib
from pathlib import Path

FOOTER_SIZE = 40
FORMAT_VERSION = 2
ABI = 3
ICON_SIZES = (16, 24, 32, 48, 64, 128, 256, 512)
MAX_MANIFEST = 4096
MAX_ICON = 4 * 1024 * 1024
MAX_ICON_PIXELS = 1024 * 1024
MAX_ENTRIES = 512
PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'
IDENT = re.compile(r'^[A-Za-z0-9._-]+$')
NAME_CHARS = re.compile(r'^[A-Za-z0-9._/-]+$')

# key -> (max length including NUL, kind)
MANIFEST_KEYS = {'id': 96, 'name': 64, 'version': 32, 'description': 128,
                 'icon': 256, 'exec': 256, 'kind': 16, 'source': 256, 'type': 16, 'abi': None}


class PackError(Exception):
    pass


def is_identifier(text):
    return bool(text) and text not in ('.', '..') and bool(IDENT.match(text))


def safe_name(name):
    if not name or len(name) > 99 or name.startswith('/') or name.endswith('/'):
        return False
    if not NAME_CHARS.match(name):
        return False
    return all(part not in ('', '.', '..') for part in name.split('/'))


def parse_manifest(data):
    """Same grammar as wiz_manifest_parse(): returns a dict of recognized keys."""
    if not data or len(data) > MAX_MANIFEST:
        raise PackError('manifest is empty or larger than %d bytes' % MAX_MANIFEST)
    values = {}
    for raw in data.split(b'\n'):
        if any((c < 32 and c not in (9, 13)) or c >= 127 for c in raw):
            raise PackError('manifest contains a non-ASCII or control character')
        if len(raw) >= 511:
            raise PackError('manifest line too long')
        line = raw.decode('ascii').strip(' \t\r')
        if not line or line.startswith('#'):
            continue
        if '=' not in line:
            raise PackError('manifest line without "=": %r' % line)
        key, value = line.split('=', 1)
        key, value = key.strip(' \t'), value.strip(' \t')
        if key not in MANIFEST_KEYS:
            continue                                  # future metadata
        if key in values:
            raise PackError('duplicate manifest key: ' + key)
        if not value:
            raise PackError('empty value for key: ' + key)
        limit = MANIFEST_KEYS[key]
        if key == 'abi':
            if not value.isdigit() or len(value) > 4:
                raise PackError('abi must be a number')
            values[key] = int(value)
        else:
            if len(value) >= limit:
                raise PackError('%s is too long (max %d)' % (key, limit - 1))
            values[key] = value
    return values


def check_package_manifest(values):
    if 'type' in values and values['type'] not in ('gui', 'console'):
        raise PackError('type must be gui or console')
    for key in ('id', 'name', 'version', 'abi'):
        if key not in values:
            raise PackError('manifest needs %s=' % key)
    if not is_identifier(values['id']) or len(values['id']) > 64:
        raise PackError('id must be 1-64 of A-Z a-z 0-9 . _ - (not "." or "..")')
    if 'icon' in values and not is_identifier(values['icon']):
        raise PackError('icon must be a theme icon name (A-Z a-z 0-9 . _ -)')
    if values['abi'] == 0:
        raise PackError('abi must be at least 1')
    if values['abi'] > ABI:
        raise PackError('package needs abi %d but this tool targets abi %d' % (values['abi'], ABI))


def check_elf(data):
    if len(data) < 64 or data[:4] != b'\x7fELF':
        raise PackError('not an ELF file')
    if data[4] != 2 or data[5] != 1:
        raise PackError('ELF must be 64-bit little-endian')
    e_type, e_machine = struct.unpack_from('<HH', data, 16)
    if e_type != 2 or e_machine != 0x3E:
        raise PackError('ELF must be a static x86-64 executable (ET_EXEC)')
    e_phoff, = struct.unpack_from('<Q', data, 32)
    e_phentsize, e_phnum = struct.unpack_from('<HH', data, 54)
    if e_phentsize != 56 or not e_phnum or e_phoff + e_phnum * 56 > len(data):
        raise PackError('ELF program headers are missing or out of range')
    for i in range(e_phnum):
        p_type, = struct.unpack_from('<I', data, e_phoff + i * 56)
        if p_type in (2, 3):                          # PT_DYNAMIC / PT_INTERP
            raise PackError('ELF is dynamically linked; WyzarOS has no dynamic linker')


def png_dimensions(data):
    if len(data) < 24 or data[:8] != PNG_SIGNATURE or data[12:16] != b'IHDR':
        raise PackError('not a PNG file')
    return struct.unpack('>II', data[16:24])


def check_icon(data, size, label):
    if len(data) > MAX_ICON:
        raise PackError('%s exceeds %d bytes' % (label, MAX_ICON))
    w, h = png_dimensions(data)
    if w * h > MAX_ICON_PIXELS or w == 0 or h == 0:
        raise PackError('%s has unsupported dimensions %dx%d' % (label, w, h))
    if (w, h) != (size, size):
        print('warning: %s is %dx%d, expected %dx%d' % (label, w, h, size, size), file=sys.stderr)


def tar_header(name, size):
    header = bytearray(512)
    encoded = name.encode('ascii')
    header[0:len(encoded)] = encoded
    header[100:108] = b'0000644\0'
    header[108:116] = b'0000000\0'
    header[116:124] = b'0000000\0'
    header[124:136] = b'%011o\0' % size
    header[136:148] = b'00000000000\0'                # mtime 0: reproducible output
    header[148:156] = b' ' * 8
    header[156] = ord('0')
    header[257:263] = b'ustar\0'
    header[263:265] = b'00'
    header[148:156] = b'%06o\0 ' % sum(header)
    return bytes(header)


def build_tar(entries):
    out = bytearray()
    for name, data in entries:
        out += tar_header(name, len(data))
        out += data
        out += b'\0' * (-len(data) % 512)
    out += b'\0' * 1024
    return bytes(out)


def assemble(elf, entries, version=FORMAT_VERSION, payload=None):
    """ELF + tar payload + footer. `payload` overrides the tar (used by tests)."""
    if payload is None:
        payload = build_tar(entries)
    body = struct.pack('<4sIQQQI', b'WIZ1', version, len(elf), len(elf), len(payload),
                       zlib.crc32(payload if version == 1 else elf + payload))
    footer = body + struct.pack('<I', zlib.crc32(body))
    assert len(footer) == FOOTER_SIZE
    return elf + payload + footer


def find_icon(directory, name, size):
    for candidate in (directory / ('%dx%d' % (size, size)) / 'apps' / (name + '.png'),
                      directory / ('%d.png' % size)):
        if candidate.is_file():
            return candidate
    return None


def pack(args):
    elf = Path(args.elf).read_bytes()
    check_elf(elf)
    manifest = Path(args.manifest).read_bytes()
    values = parse_manifest(manifest)
    check_package_manifest(values)
    entries = [('app.manifest', manifest)]

    icon_name = args.icon_name or values.get('icon') or values['id']
    if args.icons:
        directory = Path(args.icons)
        for size in ICON_SIZES:
            path = find_icon(directory, icon_name, size)
            if path:
                data = path.read_bytes()
                check_icon(data, size, str(path))
                entries.append(('icons/%d.png' % size, data))
        if len(entries) == 1:
            print('warning: no icons named %r found under %s' % (icon_name, directory), file=sys.stderr)
    if args.data:
        root = Path(args.data)
        for path in sorted(p for p in root.rglob('*') if p.is_file()):
            name = 'data/' + path.relative_to(root).as_posix()
            if not safe_name(name):
                raise PackError('data file name not allowed in a package: ' + name)
            entries.append((name, path.read_bytes()))
    if len(entries) > MAX_ENTRIES:
        raise PackError('too many entries (max %d)' % MAX_ENTRIES)
    seen = set()
    for name, _ in entries:
        if name in seen or not safe_name(name):
            raise PackError('bad or duplicate entry name: ' + name)
        seen.add(name)

    image = assemble(elf, entries)
    Path(args.output).write_bytes(image)
    print('packed %s: %s %s, %d icon(s), %d entries, %d bytes' % (
        args.output, values['name'], values['version'],
        sum(1 for n, _ in entries if n.startswith('icons/')), len(entries), len(image)))


# ── Reading ───────────────────────────────────────────────────────────────────
class Package:
    def __init__(self, data):
        self.data = data
        if len(data) < FOOTER_SIZE or data[-FOOTER_SIZE:-FOOTER_SIZE + 4] != b'WIZ1':
            raise PackError('not a .wiz package (no footer; plain ELF?)')
        raw = data[-FOOTER_SIZE:]
        if struct.unpack('<I', raw[36:])[0] != zlib.crc32(raw[:36]):
            raise PackError('footer checksum mismatch')
        magic, version, self.elf_size, self.payload_offset, self.payload_size, self.content_crc, _ = \
            struct.unpack('<4sIQQQII', raw)
        if version == 0 or version > FORMAT_VERSION:
            raise PackError('unsupported package format version %d' % version)
        self.version = version
        n = len(data)
        if (self.elf_size < 64 or self.payload_offset < self.elf_size or
                self.payload_size < 1024 or self.payload_size % 512 or
                self.payload_offset + self.payload_size + FOOTER_SIZE != n):
            raise PackError('footer is inconsistent with the file size')
        if data[:4] != b'\x7fELF':
            raise PackError('package does not begin with an ELF image')

    def entries(self):
        payload = self.data[self.payload_offset:self.payload_offset + self.payload_size]
        cursor, result = 0, []
        while True:
            if cursor + 512 > len(payload):
                raise PackError('tar has no end marker')
            header = payload[cursor:cursor + 512]
            if not any(header):
                return result
            if header[257:262] != b'ustar':
                raise PackError('tar header without ustar magic')
            stored = int(header[148:156].rstrip(b'\0 ').strip() or b'x', 8)
            if sum(header[:148]) + 256 + sum(header[156:]) != stored:
                raise PackError('tar header checksum mismatch')
            if header[345]:
                raise PackError('tar prefix field is not supported')
            kind = chr(header[156]) if header[156] else '0'
            if kind not in '05':
                raise PackError('unsupported tar entry type %r' % kind)
            name = header[:100].split(b'\0')[0].decode('ascii')
            if kind == '5':
                name = name.rstrip('/')
            if not safe_name(name):
                raise PackError('unsafe entry name: %r' % name)
            size = 0 if kind == '5' else int(header[124:136].rstrip(b'\0 ') or b'x', 8)
            padded = (size + 511) // 512 * 512
            if 512 + padded > len(payload) - cursor:
                raise PackError('tar entry runs past the payload')
            if kind == '0':
                result.append((name, payload[cursor + 512:cursor + 512 + size]))
            cursor += 512 + padded
            if len(result) > MAX_ENTRIES:
                raise PackError('too many entries')

    def verify(self):
        payload = self.data[self.payload_offset:self.payload_offset + self.payload_size]
        content = payload if self.version == 1 else self.data[:-FOOTER_SIZE]
        if zlib.crc32(content) != self.content_crc:
            raise PackError('package content checksum mismatch')
        entries = self.entries()
        # Match the runtime: the first entry with a given name wins.
        unique = {}
        for name, data in entries:
            unique.setdefault(name, data)
        manifest = unique.get('app.manifest')
        if manifest is None:
            raise PackError('package has no app.manifest')
        values = parse_manifest(manifest)
        check_package_manifest(values)
        return values, list(unique.items())


def load(path):
    return Package(Path(path).read_bytes())


def info(args):
    package = load(args.file)
    values, entries = package.verify()
    print('%s %s  (id %s, abi %d)' % (values['name'], values['version'], values['id'], values['abi']))
    if 'description' in values:
        print('  ' + values['description'])
    print('  ELF image : %d bytes' % package.elf_size)
    print('  payload   : %d bytes' % package.payload_size)
    print('  checksum  : %08x (%s)' % (package.content_crc,
          'payload only, legacy v1' if package.version == 1 else 'ELF and payload'))
    for name, data in entries:
        print('  %-28s %8d bytes' % (name, len(data)))
    print('OK')


def verify(args):
    package = load(args.file)
    package.verify()
    print('%s: OK' % args.file)


def extract(args):
    package = load(args.file)
    _, entries = package.verify()
    for name, data in entries:
        target = Path(args.directory) / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    print('extracted %d entries to %s' % (len(entries), args.directory))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('pack', help='build a .wiz package')
    p.add_argument('--elf', required=True)
    p.add_argument('--manifest', required=True)
    p.add_argument('--icons', help='icon theme directory (NxN/apps/<name>.png) or flat N.png files')
    p.add_argument('--icon-name', help='icon file name to look for (default: manifest icon, else id)')
    p.add_argument('--data', help='directory of read-only resources stored under data/')
    p.add_argument('-o', '--output', required=True)
    p.set_defaults(func=pack)
    for name, func, help_text in (('info', info, 'show manifest and contents'),
                                  ('verify', verify, 'validate structure and checksums')):
        p = sub.add_parser(name, help=help_text)
        p.add_argument('file')
        p.set_defaults(func=func)
    p = sub.add_parser('extract', help='unpack the payload')
    p.add_argument('file')
    p.add_argument('directory')
    p.set_defaults(func=extract)
    args = parser.parse_args(argv)
    try:
        args.func(args)
    except PackError as error:
        print('wiz_pack: error: %s' % error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
