"""Edit this project's FAT16 system volume inside an unpublished image copy.

Only bundled program paths are replaced. Defaults are created only when absent.
No physical devices are opened here; make_usb_image owns locking/publication.
"""
import datetime
import struct


class SystemVolume:
    def __init__(self, data):
        self.data = bytearray(data)
        b = self.data
        if (len(b) != 64 * 1024 * 1024 or b[43:54] != b'WYZARDATA  ' or
                struct.unpack_from('<H', b, 11)[0] != 512 or b[13] != 8 or
                struct.unpack_from('<H', b, 14)[0] != 1 or b[16] != 2 or
                struct.unpack_from('<H', b, 17)[0] != 512 or
                struct.unpack_from('<H', b, 22)[0] != 128 or
                struct.unpack_from('<I', b, 32)[0] != 131072 or b[510:512] != b'\x55\xaa'):
            raise ValueError('Unsupported system volume geometry')
        self.root = 257 * 512
        self.base = 289 * 512
        self.count = (131072 - 289) // 8
        if b[512:129*512] != b[129*512:257*512]:
            raise ValueError('FAT copies disagree; refusing system update')
        self.validate_tree()

    def fat(self, c):
        return struct.unpack_from('<H', self.data, 512 + c * 2)[0]

    def set_fat(self, c, value):
        for offset in (512, 129 * 512):
            struct.pack_into('<H', self.data, offset + c * 2, value)

    def chain(self, first):
        result, seen = [], set()
        while first:
            if first < 2 or first >= self.count + 2 or first in seen:
                raise ValueError('Invalid/cyclic cluster chain')
            seen.add(first)
            result.append(first)
            value = self.fat(first)
            if value >= 0xfff8:
                return result
            first = value
            if not first:
                raise ValueError('Allocated chain points to free cluster')
        return result

    def offset(self, c):
        return self.base + (c - 2) * 4096

    def slots(self, directory):
        if not directory:
            yield from range(self.root, self.root + 512 * 32, 32)
        else:
            for c in self.chain(directory):
                yield from range(self.offset(c), self.offset(c) + 4096, 32)

    def validate_tree(self):
        owned, visited = set(), set()
        pending = [0]
        while pending:
            directory = pending.pop()
            if directory in visited:
                raise ValueError('Directory cycle')
            visited.add(directory)
            for pos in self.slots(directory):
                e = self.data[pos:pos+32]
                if not e[0]:
                    break
                if e[0] == 0xe5 or e[11] == 15 or e[11] & 8 or e[0] == 46:
                    continue
                first, size = struct.unpack_from('<HI', e, 26)
                chain = self.chain(first)
                if owned.intersection(chain):
                    raise ValueError('Cross-linked filesystem; refusing update')
                owned.update(chain)
                if e[11] & 16:
                    if not first:
                        raise ValueError('Directory without storage')
                    pending.append(first)
                elif size > len(chain) * 4096:
                    raise ValueError('File size exceeds cluster chain')

    @staticmethod
    def name(name):
        parts = name.upper().split('.')
        if (len(parts) > 2 or not 1 <= len(parts[0]) <= 8 or
                (len(parts) == 2 and not 1 <= len(parts[1]) <= 3)):
            raise ValueError('System paths must be 8.3')
        return parts[0].ljust(8).encode('ascii') + (parts[1] if len(parts) == 2 else '').ljust(3).encode('ascii')

    def find(self, directory, name):
        short = self.name(name)
        long_entry = False
        for pos in self.slots(directory):
            e = self.data[pos:pos+32]
            if not e[0]:
                return None
            if e[0] != 0xe5 and e[:11] == short:
                if long_entry or e[11] & (8 | 1):
                    raise ValueError('Protected or long-name system path')
                return pos
            long_entry = e[0] != 0xe5 and e[11] == 15
        return None

    def allocate(self, content):
        count = (len(content) + 4095) // 4096
        free = []
        for c in range(2, self.count + 2):
            if len(free) == count:
                break
            if self.fat(c) == 0:
                free.append(c)
        if len(free) != count:
            raise ValueError('System volume full; original image left unchanged')
        for i, c in enumerate(free):
            self.set_fat(c, free[i+1] if i+1 < count else 0xffff)
            chunk = content[i*4096:(i+1)*4096]
            pos = self.offset(c)
            self.data[pos:pos+4096] = chunk.ljust(4096, b'\0')
        return free[0] if free else 0

    def entry(self, directory, name, content, is_dir=False, default=False):
        pos = self.find(directory, name)
        if pos is not None:
            if bool(self.data[pos+11] & 16) != is_dir:
                raise ValueError('System file/directory conflict')
            if default or is_dir:
                return struct.unpack_from('<H', self.data, pos+26)[0]
            for c in self.chain(struct.unpack_from('<H', self.data, pos+26)[0]):
                self.set_fat(c, 0)
        else:
            pos = next((p for p in self.slots(directory) if self.data[p] in (0, 0xe5)), None)
            if pos is None:
                raise ValueError('System directory full; original image left unchanged')
        cluster = self.allocate(content)
        e = bytearray(32)
        e[:11] = self.name(name)
        e[11] = 16 if is_dir else 32
        now = datetime.datetime.now()
        date = ((max(1980, min(now.year, 2107))-1980) << 9) | (now.month << 5) | now.day
        time = (now.hour << 11) | (now.minute << 5) | now.second // 2
        struct.pack_into('<HHH', e, 14, time, date, date)
        struct.pack_into('<HHHI', e, 22, time, date, cluster, 0 if is_dir else len(content))
        self.data[pos:pos+32] = e
        if is_dir:
            dots = bytearray(64)
            dots[:11], dots[32:43] = b'.          ', b'..         '
            dots[11] = dots[43] = 16
            struct.pack_into('<H', dots, 26, cluster)
            struct.pack_into('<H', dots, 58, directory)
            start = self.offset(cluster)
            self.data[start:start+64] = dots
        return cluster

    def mkdir(self, path):
        directory = 0
        for part in path.split('/'):
            if part:
                directory = self.entry(directory, part, bytes(4096), is_dir=True)
        return directory

    def put(self, path, content, default=False):
        parent, _, name = path.rpartition('/')
        self.entry(self.mkdir(parent), name, content, default=default)

    def read(self, path):
        directory = 0
        parts = path.split('/')
        for part in parts[:-1]:
            pos = self.find(directory, part)
            if pos is None or not self.data[pos+11] & 16:
                return None
            directory = struct.unpack_from('<H', self.data, pos+26)[0]
        pos = self.find(directory, parts[-1])
        if pos is None:
            return None
        first, size = struct.unpack_from('<HI', self.data, pos+26)
        return b''.join(self.data[self.offset(c):self.offset(c)+4096] for c in self.chain(first))[:size]


def install_system(data, root):
    volume = SystemVolume(data)
    for directory in ('bin', 'etc', 'home', 'share', 'tmp', 'run', 'dev', 'mnt'):
        volume.mkdir(directory)
    for program in ('init.elf', 'counter.elf', 'win_test.elf'):
        volume.put('bin/' + program, (root / program).read_bytes())
    volume.put('etc/root.cfg', b'WYZARROOT1\n')
    volume.put('etc/desktop.cfg', b'/share/wall.raw\n', default=True)
    volume.put('bootlog.txt', b'', default=True)
    volume.put('run/boot.log', b'', default=True)
    if (root / 'wallpaper.raw').exists():
        volume.put('share/wall.raw', (root / 'wallpaper.raw').read_bytes(), default=True)
    font_license = root / 'assets/fonts/selawik/LICENSE.txt'
    if font_license.exists():
        volume.put('share/fonts/selawik.txt', font_license.read_bytes())
    volume.validate_tree()
    return volume.data
