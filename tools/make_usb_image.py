#!/usr/bin/env python3
"""Build a UEFI USB image, optionally refreshing boot files while retaining data.

New images have a FAT EFI boot partition and a journaled ext4 WYZARDATA root.
Creation/update requires WSL/Linux, e2fsprogs and a C compiler. No devices opened.
"""
import argparse
from pathlib import Path
import struct
import datetime
import os
import tempfile
import stat
import zlib
from contextlib import contextmanager
from ext4_volume import install_ext4_file, EXT4_SECTORS, MIN_SECTORS, MAX_SECTORS, check_header
from image_io import copy_sparse
from types import SimpleNamespace

SECTOR = 512
START = 2048
BOOT_SECTORS = 262144


class Fat16Image:
    """Build the firmware boot partition; never used as a system root."""
    def __init__(self, sectors, label, hidden):
        self.data = bytearray(sectors * SECTOR)
        self.spc, self.fatsecs, self.roots = 8, 128, 512
        self.root_sector = 1 + 2 * self.fatsecs
        self.data_sector = self.root_sector + 32
        self.clusters = (sectors - self.data_sector) // self.spc
        assert 4085 <= self.clusters < 65525
        self.next_cluster = 2
        now = datetime.datetime.now()
        self.date = ((max(1980, min(2107, now.year)) - 1980) << 9) | (now.month << 5) | now.day
        self.time = (now.hour << 11) | (now.minute << 5) | (now.second // 2)
        self.fraction = 0 # match FAT modification time's two-second precision
        b = self.data
        b[:3] = b'\xeb\x3c\x90'
        b[3:11] = b'WYZAROS '
        struct.pack_into('<HBHBHHBHHHII', b, 11, 512, self.spc, 1, 2,
                         self.roots, 0, 0xf8, self.fatsecs, 63, 255, hidden, sectors)
        b[36] = 0x80
        b[38] = 0x29
        struct.pack_into('<I', b, 39, 0x57595a01)
        b[43:54] = label.encode('ascii').ljust(11, b' ')
        b[54:62] = b'FAT16   '
        b[510:512] = b'\x55\xaa'
        self.set_fat(0, 0xfff8)
        self.set_fat(1, 0xffff)
        self.dirs = {'': (0, 1)} # cluster and next directory slot (label uses 0)
        pos = self.root_sector * SECTOR
        b[pos:pos + 11] = b[43:54]
        b[pos + 11] = 8

    def set_fat(self, cluster, value):
        for copy in range(2):
            struct.pack_into('<H', self.data, (1 + copy * self.fatsecs) * SECTOR + cluster * 2, value)

    def allocate(self, contents):
        count = max(1, (len(contents) + 4095) // 4096)
        first = self.next_cluster
        if first + count > self.clusters + 2:
            raise ValueError('Boot files exceed image capacity')
        self.next_cluster += count
        for i in range(count):
            self.set_fat(first + i, first + i + 1 if i + 1 < count else 0xffff)
        pos = (self.data_sector + (first - 2) * self.spc) * SECTOR
        self.data[pos:pos + len(contents)] = contents
        return first

    @staticmethod
    def short_name(name):
        parts = name.upper().split('.')
        if len(parts) > 2 or not 1 <= len(parts[0]) <= 8 or (len(parts) == 2 and not 1 <= len(parts[1]) <= 3):
            raise ValueError(f'Not an 8.3 filename: {name}')
        return parts[0].ljust(8).encode() + (parts[1] if len(parts) == 2 else '').ljust(3).encode()

    def entry(self, parent, name, cluster, size, attr):
        try:
            short = self.short_name(name)
            long_entries = []
        except ValueError:
            # Boot assets retain their Limine paths using VFAT long entries.
            base, _, ext = name.upper().rpartition('.')
            short = (base[:6] + '~1').ljust(8).encode() + ext[:3].ljust(3).encode()
            checksum = 0
            for value in short:
                checksum = (((checksum & 1) << 7) + (checksum >> 1) + value) & 255
            units = list(struct.unpack('<' + 'H' * len(name), name.encode('utf-16le'))) + [0]
            units += [0xffff] * ((-len(units)) % 13)
            long_entries = []
            count = len(units) // 13
            for part in range(count, 0, -1):
                e = bytearray(32)
                e[0] = part | (0x40 if part == count else 0)
                e[11] = 0x0f
                e[13] = checksum
                for offset, unit in zip((1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30), units[(part - 1) * 13:part * 13]):
                    struct.pack_into('<H', e, offset, unit)
                long_entries.append(e)
        c, index = self.dirs[parent]
        limit = 128 if c else self.roots
        if index + len(long_entries) >= limit:
            raise ValueError('Image directory full')
        sector = self.data_sector + (c - 2) * self.spc if c else self.root_sector
        pos = sector * SECTOR + index * 32
        for e in long_entries:
            self.data[pos:pos + 32] = e
            pos += 32
            index += 1
        self.data[pos:pos + 11] = short
        self.data[pos + 11] = attr
        self.data[pos + 13] = self.fraction
        struct.pack_into('<HHH', self.data, pos + 14, self.time, self.date, self.date)
        struct.pack_into('<HHHI', self.data, pos + 22, self.time, self.date, cluster, size)
        self.dirs[parent] = c, index + 1

    def mkdir(self, path):
        if path in self.dirs:
            return
        parent, _, name = path.rpartition('/')
        self.mkdir(parent)
        dots = bytearray(4096)
        dots[:11] = b'.          '
        dots[32:43] = b'..         '
        dots[11] = dots[43] = 0x10
        struct.pack_into('<H', dots, 26, self.next_cluster)
        struct.pack_into('<H', dots, 58, self.dirs[parent][0])
        cluster = self.allocate(dots)
        self.entry(parent, name, cluster, 0, 0x10)
        self.dirs[path] = cluster, 2

    def add(self, path, contents):
        parent, _, name = path.rpartition('/')
        self.mkdir(parent)
        cluster = self.allocate(contents) if contents else 0
        self.entry(parent, name, cluster, len(contents), 0x20)


@contextmanager
def locked_image(path):
    if path.is_symlink() or not stat.S_ISREG(path.stat().st_mode):
        raise ValueError('Boot updates require a regular image file, not a device or symlink')
    with path.open('r+b') as source:
        length = os.fstat(source.fileno()).st_size
        try:
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(source.fileno(), msvcrt.LK_NBLCK, length)
            else:
                import fcntl
                fcntl.lockf(source, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise RuntimeError('Image is in use. Close QEMU before rebuilding it.') from exc
        try:
            yield source
        finally:
            source.seek(0)
            if os.name == 'nt':
                msvcrt.locking(source.fileno(), msvcrt.LK_UNLCK, length)
            else:
                fcntl.lockf(source, fcntl.LOCK_UN)


def validate_existing(source, allow_trailing=False):
    source.seek(0)
    mbr = source.read(SECTOR)
    if mbr[510:512] != b'\x55\xaa':
        raise ValueError('Invalid image MBR')
    data_count = struct.unpack_from('<I', mbr, 474)[0]
    if data_count % 8 or not MIN_SECTORS <= data_count <= MAX_SECTORS:
        raise ValueError('Unrecognized system partition size')
    actual = os.fstat(source.fileno()).st_size
    expected = (START + BOOT_SECTORS + data_count) * SECTOR
    if actual < expected or (actual != expected and not allow_trailing):
        raise ValueError('Unrecognized image size; refusing boot update')
    for i, (kind, start, count, label) in enumerate((
        (0xef, START, BOOT_SECTORS, b'WYZARBOOT  '),
        (0x83, START + BOOT_SECTORS, data_count, b'WYZARDATA  '),
    )):
        offset = 446 + 16 * i
        if mbr[offset + 4] != kind or struct.unpack_from('<II', mbr, offset + 8) != (start, count):
            raise ValueError('Unrecognized partition layout; refusing boot update')
        source.seek(start * SECTOR)
        if i == 1:
            source.seek(start * SECTOR + 1024)
            check_header(source.read(1024), data_count * SECTOR)
            continue
        b = source.read(SECTOR)
        if b[43:54] != label or b[510:512] != b'\x55\xaa':
            raise ValueError('Unrecognized volume label or boot sector; refusing boot update')
    for i in (2, 3):
        if any(mbr[446 + 16*i:462 + 16*i]):
            raise ValueError('Extra partitions present; refusing boot update')
    return data_count


def storage_policy(uuid, sectors, automatic):
    result = bytearray(512)
    struct.pack_into('<16sIIQQ16s', result, 0, b'WYZAR-AUTOGROW1', 1, int(automatic), START + BOOT_SECTORS, sectors, uuid)
    struct.pack_into('<I', result, 508, zlib.crc32(result[:508]))
    return result


def write_image(output, boot, data, mbr, update_boot=False, fresh=False, system_root=None, policy=None):
    # Atomic replacement: a failed refresh leaves the previous complete image.
    # Lock the old image against QEMU's sector writes throughout the copy.
    exists = output.exists() or output.is_symlink()
    if exists and not (update_boot or fresh):
        raise FileExistsError(f'{output} already exists; choose a new --output or use --update-boot')
    temp_path = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=output.name + '.', suffix='.tmp', delete=False) as dest:
            temp_path = Path(dest.name)
            if exists:
                with locked_image(output) as source:
                    if fresh:
                        dest.write(mbr)
                        if policy is not None:
                            dest.seek(512); dest.write(policy)
                        dest.seek(START * SECTOR)
                        dest.write(boot.data)
                        write_partition(dest, data)
                    else:
                        data_count = validate_existing(source)
                        source.seek(0)
                        dest.write(source.read(START * SECTOR))
                        if policy is not None:
                            dest.seek(512); dest.write(policy)
                            dest.seek(START * SECTOR)
                        dest.write(boot.data)
                        source.seek((START + BOOT_SECTORS) * SECTOR)
                        if system_root:
                            with tempfile.TemporaryDirectory(prefix='wyzar-update-') as directory:
                                volume = Path(directory) / 'volume.img'
                                with volume.open('wb') as private:
                                    copy_sparse(source, private, data_count * SECTOR)
                                install_ext4_file(volume, system_root)
                                with volume.open('rb') as private:
                                    copy_sparse(private, dest, data_count * SECTOR, sparse=data_count >= 1048576)
                        else:
                            copy_sparse(source, dest, data_count * SECTOR, sparse=data_count >= 1048576)
                    dest.flush()
                    os.fsync(dest.fileno())
                    # Keep lock held until the complete replacement is ready.
                    if os.name != 'nt':
                        os.replace(temp_path, output)
                        temp_path = None
            else:
                dest.write(mbr)
                if policy is not None:
                    dest.seek(512); dest.write(policy)
                dest.seek(START * SECTOR)
                dest.write(boot.data)
                write_partition(dest, data)
                dest.flush()
                os.fsync(dest.fileno())
        if temp_path:
            if exists:
                os.replace(temp_path, output)
            else:
                # Exclusive publication also handles a concurrent creator.
                os.link(temp_path, output)
                temp_path.unlink()
            temp_path = None
    finally:
        if temp_path and temp_path.exists():
            temp_path.unlink()


def write_partition(destination, data):
    if hasattr(data, 'path'):
        with data.path.open('rb') as source:
            length = data.path.stat().st_size
            copy_sparse(source, destination, length, sparse=length >= 512 * 1024 * 1024)
    else:
        destination.write(data.data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('wyzar-usb.img'))
    parser.add_argument('--root-mib', type=int, help='Root capacity for new/fresh images (default 256 MiB)')
    parser.add_argument('--debug', action='store_true', help='Fixed 256 MiB root; skip automatic first-boot setup')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--update-boot', action='store_true', help='Refresh an existing image atomically, refreshing bundled system apps and preserving user files/settings; create it if missing')
    mode.add_argument('--fresh', action='store_true', help='Replace the complete image with a fresh one, ERASING saved files (refuses images in use)')
    args = parser.parse_args()
    if args.debug and args.root_mib not in (None, 256):
        parser.error('--debug uses a fixed 256 MiB root')
    root = Path(__file__).resolve().parent.parent
    files = {
        'EFI/BOOT/BOOTX64.EFI': 'limine/BOOTX64.EFI',
        'limine.conf': 'limine.conf',
        'boot/wyzar': 'wyzar',
        'boot/initrd.tar': 'initrd.tar',
        'boot/init.elf': 'init.elf',
    }
    boot = Fat16Image(BOOT_SECTORS, 'WYZARBOOT', START)
    for target, source in files.items():
        boot.add(target, (root / source).read_bytes())
    data_start = START + BOOT_SECTORS
    updating = args.output.exists() and args.update_boot and not args.fresh
    data_count = (args.root_mib * 2048) if args.root_mib is not None else EXT4_SECTORS
    if not MIN_SECTORS <= data_count <= MAX_SECTORS:
        parser.error('Root capacity must be at least 64 MiB and fit the MBR/READ(10) layout')
    if updating:
        with locked_image(args.output) as source:
            existing = validate_existing(source)
        if args.root_mib is not None and data_count != existing:
            parser.error('Updates preserve root capacity; use expand_usb_image.py to grow an existing image')
        data_count = existing
        if args.debug and existing != EXT4_SECTORS:
            parser.error('DEBUG=1 needs a 256 MiB root; use FRESH=1 to reset a larger image')
    mbr = bytearray(SECTOR)
    for i, (kind, start, count) in enumerate(((0xef, START, BOOT_SECTORS), (0x83, data_start, data_count))):
        struct.pack_into('<B3sB3sII', mbr, 446 + i * 16, 0, b'\xfe\xff\xff', kind, b'\xfe\xff\xff', start, count)
    mbr[510:512] = b'\x55\xaa'
    existed = args.output.exists()
    with tempfile.TemporaryDirectory(prefix='wyzar-new-image-') as directory:
        data = None
        if not updating:
            volume = Path(directory) / 'volume.img'
            install_ext4_file(volume, root, create=True, sectors=data_count)
            data = SimpleNamespace(path=volume)
            with volume.open('rb') as source:
                source.seek(1024 + 104); uuid = source.read(16)
        else:
            with locked_image(args.output) as source:
                source.seek(data_start * SECTOR + 1024 + 104); uuid = source.read(16)
        policy = storage_policy(uuid, data_count, not args.debug)
        write_image(args.output, boot, data, mbr, args.update_boot, args.fresh, system_root=root, policy=policy)
    preserved = existed and not args.fresh
    print(f'{"Updated boot files in" if preserved else "Created fresh image"} {args.output}; ' +
          ('ext4 bundled apps refreshed; user files/settings/wallpaper preserved' if preserved else
           f'UEFI boot + {data_count // 2048} MiB ext4 seed root'))
    print('Storage mode: fixed 256 MiB debug' if args.debug else 'Storage mode: automatic first-boot setup on larger USB media')


if __name__ == '__main__':
    main()
