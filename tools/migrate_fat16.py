"""Validated FAT16 export to a new ext4 image, without altering the source."""
from pathlib import Path
import struct
import tempfile
from system_volume import SystemVolume
from ext4_volume import install_ext4


def migrate(source_path, root):
    from make_usb_image import locked_image, validate_existing, START, BOOT_SECTORS, SECTOR
    with locked_image(source_path) as source:
        filesystem, count = validate_existing(source)
        if filesystem != 'fat16':
            raise ValueError('Migration source must be a recognized FAT16 image')
        source.seek((START + BOOT_SECTORS) * SECTOR)
        volume = SystemVolume(source.read(count * SECTOR))
    with tempfile.TemporaryDirectory(prefix='wyzar-fat-export-') as temporary:
        tree = Path(temporary)

        def export(directory, parent):
            for pos in volume.slots(directory):
                e = volume.data[pos:pos + 32]
                if not e[0]:
                    break
                if e[0] == 0xe5 or e[11] & 8 and e[11] != 15 or e[0] == 46:
                    continue
                if e[11] == 15:
                    raise ValueError('Long-name FAT entries need a separate migration path; source unchanged')
                base = bytes(e[:8]).decode('ascii').rstrip(' ')
                ext = bytes(e[8:11]).decode('ascii').rstrip(' ')
                name = (base + ('.' + ext if ext else '')).lower()
                if not name or any(c in name for c in '/\\\x00') or name in ('.', '..'):
                    raise ValueError('Invalid FAT filename; source unchanged')
                target = parent / name
                if target.exists():
                    raise ValueError('Duplicate FAT filename; source unchanged')
                first, size = struct.unpack_from('<HI', e, 26)
                if e[11] & 16:
                    target.mkdir()
                    export(first, target)
                else:
                    target.write_bytes(b''.join(volume.data[volume.offset(c):volume.offset(c)+4096]
                                               for c in volume.chain(first))[:size])

        export(0, tree)
        # FAT paths are case-insensitive; our new ext4 tree uses lowercase short
        # names. Preserve the selected assets when these known settings use caps.
        for setting in ('etc/desktop.cfg', 'etc/font.cfg'):
            path = tree / setting
            if path.is_file():
                content = path.read_bytes()
                asset = content.strip()
                try:
                    relative = asset.decode('ascii').lstrip('/').lower()
                except UnicodeDecodeError:
                    continue
                if asset.startswith(b'/') and '..' not in Path(relative).parts and (tree / relative).is_file():
                    path.write_bytes(content.replace(asset, asset.lower(), 1))
        return install_ext4(None, root, tree)
