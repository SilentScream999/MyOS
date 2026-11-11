#!/usr/bin/env python3
"""Grow an offline regular USB image atomically. Never opens physical devices."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile
from ext4_volume import MIN_SECTORS, MAX_SECTORS, check_file
from image_io import copy_sparse
from make_usb_image import START, BOOT_SECTORS, locked_image, validate_existing


def grow_image(path, sectors=None, use_file_space=False):
    path = Path(path)
    temporary = None
    start = START + BOOT_SECTORS
    try:
        with locked_image(path) as source:
            current = validate_existing(source, allow_trailing=use_file_space)
            if use_file_space:
                sectors = ((os.fstat(source.fileno()).st_size // 512) - start) & ~7
            if sectors is None or sectors % 8 or not MIN_SECTORS <= sectors <= MAX_SECTORS or sectors < current:
                raise ValueError('Expansion must not shrink and must fit the supported MBR layout')
            if sectors == current:
                return sectors
            with tempfile.TemporaryDirectory(prefix='wyzar-grow-') as directory:
                volume = Path(directory) / 'volume.img'
                source.seek(start * 512)
                with volume.open('wb') as destination:
                    copy_sparse(source, destination, current * 512)
                result = subprocess.run(['e2fsck', '-p', str(volume)], capture_output=True)
                if result.returncode not in (0, 1):
                    raise ValueError('Cannot expand an inconsistent filesystem: ' + result.stdout.decode(errors='replace'))
                with volume.open('r+b') as destination:
                    destination.truncate(sectors * 512)
                subprocess.run(['resize2fs', str(volume), str(sectors // 8)], check=True, capture_output=True)
                check_file(volume)
                subprocess.run(['e2fsck', '-fn', str(volume)], check=True, capture_output=True)
                with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + '.', suffix='.tmp', delete=False) as destination:
                    temporary = Path(destination.name)
                    source.seek(0)
                    copy_sparse(source, destination, start * 512)
                    destination.seek(474)
                    destination.write(struct.pack('<I', sectors))
                    destination.seek(start * 512)
                    with volume.open('rb') as partition:
                        copy_sparse(partition, destination, sectors * 512)
                    destination.flush()
                    os.fsync(destination.fileno())
                with temporary.open('rb') as candidate:
                    validate_existing(candidate)
                os.replace(temporary, path)
                temporary = None
            return sectors
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    size = parser.add_mutually_exclusive_group(required=True)
    size.add_argument('--root-mib', type=int, help='New system-root size in MiB')
    size.add_argument('--total-mib', type=int, help='Total image size, including the 129 MiB boot/prefix area')
    size.add_argument('--total-bytes', type=int, help='Exact target-stick capacity in bytes; rounds down to a filesystem block')
    size.add_argument('--use-file-space', action='store_true', help='Use trailing space already present in the image file')
    args = parser.parse_args()
    sectors = None
    if args.root_mib is not None:
        sectors = args.root_mib * 2048
    elif args.total_mib is not None:
        sectors = (args.total_mib * 2048 - START - BOOT_SECTORS) & ~7
    elif args.total_bytes is not None:
        sectors = (args.total_bytes // 512 - START - BOOT_SECTORS) & ~7
    result = grow_image(args.image, sectors, args.use_file_space)
    print(f'ext4 root: {result // 2048} MiB; image ready for whole-image flashing')


if __name__ == '__main__':
    main()
