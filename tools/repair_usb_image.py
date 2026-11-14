#!/usr/bin/env python3
"""Repair an offline USB image into a separate file, preserving the original.

Requires WSL/Linux e2fsprogs. Repairs may remove invalid entries or reconnect
files under /lost+found; inspect the checker report before using the result.
Never opens physical devices or overwrites an existing destination.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from ext4_volume import check_file
from image_io import copy_sparse
from make_usb_image import START, BOOT_SECTORS, locked_image, validate_existing


def check_volume(volume, repair):
    flags = ['-f', '-y'] if repair else ['-f', '-n']
    result = subprocess.run(['e2fsck', *flags, str(volume)], capture_output=True)
    stdout = result.stdout.decode(errors='replace')
    stderr = result.stderr.decode(errors='replace')
    if stdout:
        print(stdout, end='' if stdout.endswith('\n') else '\n')
    if stderr:
        print(stderr, end='' if stderr.endswith('\n') else '\n', file=sys.stderr)
    # Repair exit bits 1/2 mean corrected/reboot requested; a second read-only
    # check must still pass before this offline candidate can be published.
    if result.returncode not in ((0, 1, 2, 3) if repair else (0,)):
        raise ValueError(f'e2fsck {"repair" if repair else "verification"} failed '
                         f'(exit {result.returncode}): {stdout}{stderr}')


def repair_image(image, output=None):
    image = Path(image)
    output = Path(output) if output is not None else image.with_suffix('.recovered.img')
    if image.resolve() == output.resolve():
        raise ValueError('Recovery output must differ from the original image')
    if output.exists() or output.is_symlink():
        raise FileExistsError(f'Recovery output already exists: {output}')
    if not shutil.which('e2fsck'):
        raise RuntimeError('Recovery requires WSL/Linux with e2fsprogs (e2fsck)')
    start = (START + BOOT_SECTORS) * 512
    with locked_image(image) as source:
        sectors = validate_existing(source)
        with tempfile.TemporaryDirectory(prefix='wyzar-recover-', dir=output.parent) as directory:
            volume = Path(directory) / 'volume.img'
            source.seek(start)
            with volume.open('wb') as destination:
                copy_sparse(source, destination, sectors * 512)
            check_volume(volume, repair=True)
            check_file(volume)
            check_volume(volume, repair=False)
            candidate = Path(directory) / 'recovered.img'
            with candidate.open('wb') as destination:
                source.seek(0)
                copy_sparse(source, destination, start)
                with volume.open('rb') as partition:
                    copy_sparse(partition, destination, sectors * 512)
                destination.flush()
                os.fsync(destination.fileno())
            with candidate.open('rb') as destination:
                validate_existing(destination)
            # Exclusive publication: even a destination created during repair
            # must remain untouched. Temporary files share its filesystem.
            os.link(candidate, output)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--output', type=Path, help='New image path (default: IMAGE_STEM.recovered.img)')
    args = parser.parse_args()
    try:
        output = repair_image(args.image, args.output)
    except (OSError, RuntimeError, ValueError) as error:
        parser.exit(1, f'Recovery failed: {error}\nOriginal image preserved.\n')
    print(f'Recovered image: {output}; original preserved: {args.image}')
    print(f'Inspect the report above and /lost+found. To build with the recovered image: '
          f'make iso PERSIST_DISK="{output}"')


if __name__ == '__main__':
    main()
