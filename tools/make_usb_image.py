#!/usr/bin/env python3
"""Build a UEFI USB image, optionally refreshing boot files while retaining data.

Uses only Python's standard library. Boot partition and writable WYZARDATA
partition are separate FAT16 volumes. Flash the completed image with your usual
imaging program. Reflashing replaces the data partition too; keep backups.
"""
import argparse
from pathlib import Path
import struct
import datetime
import os
import shutil
import tempfile
import stat
from contextlib import contextmanager

SECTOR = 512
START = 2048
BOOT_SECTORS = 262144
DATA_SECTORS = 131072


class Fat16Image:
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
        struct.pack_into('<I', b, 39, 0x57595a01 if hidden == START else 0x57595a02)
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
            # The writable volume uses only 8.3 names in this first driver.
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


def validate_existing(source):
    expected_size = (START + BOOT_SECTORS + DATA_SECTORS) * SECTOR
    if os.fstat(source.fileno()).st_size != expected_size:
        raise ValueError('Unrecognized image size; refusing boot update')
    source.seek(0)
    mbr = source.read(SECTOR)
    if mbr[510:512] != b'\x55\xaa':
        raise ValueError('Invalid image MBR')
    for i, (kind, start, count, label) in enumerate((
        (0xef, START, BOOT_SECTORS, b'WYZARBOOT  '),
        (0x0e, START + BOOT_SECTORS, DATA_SECTORS, b'WYZARDATA  '),
    )):
        offset = 446 + 16 * i
        if mbr[offset + 4] != kind or struct.unpack_from('<II', mbr, offset + 8) != (start, count):
            raise ValueError('Unrecognized partition layout; refusing boot update')
        source.seek(start * SECTOR)
        b = source.read(SECTOR)
        if b[43:54] != label or b[510:512] != b'\x55\xaa':
            raise ValueError('Unrecognized volume label or boot sector; refusing boot update')
    for i in (2, 3):
        if any(mbr[446 + 16*i:462 + 16*i]):
            raise ValueError('Extra partitions present; refusing boot update')


def write_image(output, boot, data, mbr, update_boot=False, fresh=False):
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
                        dest.seek(START * SECTOR)
                        dest.write(boot.data)
                        dest.write(data.data)
                    else:
                        validate_existing(source)
                        source.seek(0)
                        dest.write(source.read(START * SECTOR))
                        dest.write(boot.data)
                        source.seek((START + BOOT_SECTORS) * SECTOR)
                        shutil.copyfileobj(source, dest)
                    dest.flush()
                    os.fsync(dest.fileno())
                    # Keep lock held until the complete replacement is ready.
                    if os.name != 'nt':
                        os.replace(temp_path, output)
                        temp_path = None
            else:
                dest.write(mbr)
                dest.seek(START * SECTOR)
                dest.write(boot.data)
                dest.write(data.data)
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('wyzar-usb.img'))
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--update-boot', action='store_true', help='Refresh an existing image atomically, preserving the complete data partition; create it if missing')
    mode.add_argument('--fresh', action='store_true', help='Replace the complete image with a fresh one, ERASING saved /disk files (refuses images in use)')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    files = {
        'EFI/BOOT/BOOTX64.EFI': 'limine/BOOTX64.EFI',
        'limine.conf': 'limine.conf',
        'boot/wyzar': 'wyzar',
        'boot/initrd.tar': 'initrd.tar',
        'boot/init.elf': 'init.elf',
    }
    if (root / 'wallpaper.raw').exists():
        files['boot/wallpaper.raw'] = 'wallpaper.raw'
    boot = Fat16Image(BOOT_SECTORS, 'WYZARBOOT', START)
    for target, source in files.items():
        boot.add(target, (root / source).read_bytes())
    data_start = START + BOOT_SECTORS
    data = Fat16Image(DATA_SECTORS, 'WYZARDATA', data_start)
    data.add('README.TXT', b'WyzarOS persistent data volume. Mounted at /disk.\r\n')
    mbr = bytearray(SECTOR)
    for i, (kind, start, count) in enumerate(((0xef, START, BOOT_SECTORS), (0x0e, data_start, DATA_SECTORS))):
        struct.pack_into('<B3sB3sII', mbr, 446 + i * 16, 0, b'\xfe\xff\xff', kind, b'\xfe\xff\xff', start, count)
    mbr[510:512] = b'\x55\xaa'
    existed = args.output.exists()
    write_image(args.output, boot, data, mbr, args.update_boot, args.fresh)
    preserved = existed and not args.fresh
    print(f'{"Updated boot files in" if preserved else "Created fresh image"} {args.output}; ' +
          ('existing /disk data preserved' if preserved else 'UEFI boot + clean 64 MiB FAT16 data volume'))


if __name__ == '__main__':
    main()
