#!/usr/bin/env python3
"""Create, expand and refresh sparse ext4 roots with bounded address space."""
from pathlib import Path
import hashlib
import resource
import subprocess
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from expand_usb_image import grow_image
from image_io import copy_sparse
from make_usb_image import validate_existing, START, BOOT_SECTORS


def bounded():
    resource.setrlimit(resource.RLIMIT_AS, (384 * 1024 * 1024,) * 2)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            h.update(block)
    return h.digest()


def main():
    with tempfile.TemporaryDirectory(prefix='wyzar-capacity-') as directory:
        work = Path(directory)
        image = work / 'system.img'
        volume = work / 'partition.img'
        offset = (START + BOOT_SECTORS) * 512
        subprocess.run([sys.executable, 'tools/make_usb_image.py', '--output', str(image), '--fresh', '--root-mib', '64'],
                       cwd=ROOT, preexec_fn=bounded, check=True)
        with image.open('rb') as source, volume.open('wb') as private:
            source.seek(offset); copy_sparse(source, private, 64 * 1024 * 1024)
        asset = work / 'asset'; asset.write_bytes(b'capacity-survived')
        manifest = work / 'manifest'; manifest.write_text(f'put /home/capacity.txt {asset}\n')
        subprocess.run([str(ROOT / '.storage-test/ext4-image-tool'), str(volume), str(manifest)], check=True)
        with volume.open('rb') as private, image.open('r+b') as destination:
            destination.seek(offset); copy_sparse(private, destination, volume.stat().st_size)
        subprocess.run([sys.executable, 'tools/expand_usb_image.py', str(image), '--root-mib', '1024'],
                       cwd=ROOT, preexec_fn=bounded, check=True)
        subprocess.run([sys.executable, 'tools/make_usb_image.py', '--output', str(image), '--update-boot'],
                       cwd=ROOT, preexec_fn=bounded, check=True)
        with image.open('rb') as source:
            assert validate_existing(source) == 1024 * 2048
        with image.open('r+b') as destination:
            destination.truncate(offset + 1536 * 1024 * 1024)
        assert grow_image(image, use_file_space=True) == 1536 * 2048
        with image.open('rb') as source, volume.open('wb') as private:
            assert validate_existing(source) == 1536 * 2048
            source.seek(offset); copy_sparse(source, private, 1536 * 1024 * 1024)
        data = subprocess.check_output(['debugfs', '-R', 'cat /home/capacity.txt', str(volume)], stderr=subprocess.DEVNULL)
        assert data == b'capacity-survived'
        subprocess.run(['e2fsck', '-fn', str(volume)], check=True, capture_output=True)
        before = digest(image)
        try:
            grow_image(image, 64 * 2048)
            raise AssertionError('Shrink accepted')
        except ValueError:
            pass
        original_run = subprocess.run

        def fail_resize(command, **kwargs):
            if command[0] == 'resize2fs':
                raise subprocess.CalledProcessError(1, command)
            return original_run(command, **kwargs)

        with patch('expand_usb_image.subprocess.run', side_effect=fail_resize):
            try:
                grow_image(image, 2048 * 2048)
                raise AssertionError('Failed resize accepted')
            except subprocess.CalledProcessError:
                pass
        assert digest(image) == before
        link = work / 'linked.img'; link.symlink_to(image)
        try:
            grow_image(link, 2048 * 2048)
            raise AssertionError('Symlink accepted')
        except ValueError:
            pass
        subprocess.run([sys.executable, 'tools/make_usb_image.py', '--output', str(image), '--fresh'],
                       cwd=ROOT, preexec_fn=bounded, check=True)
        with image.open('rb') as source:
            assert validate_existing(source) == 1536 * 2048, 'Fresh reset must retain an expanded layout'
    # Workspace output exercises the dense DrvFS path separately from native
    # Linux sparse images. It must be readable immediately after publication.
    with tempfile.TemporaryDirectory(prefix='dense-capacity-', dir=ROOT / '.storage-test') as directory:
        image = Path(directory) / 'system.img'
        subprocess.run([sys.executable, 'tools/make_usb_image.py', '--output', str(image), '--fresh', '--root-mib', '64'],
                       cwd=ROOT, preexec_fn=bounded, check=True)
        subprocess.run([sys.executable, 'tools/expand_usb_image.py', str(image), '--total-bytes', str((129 + 128) * 1024 * 1024)],
                       cwd=ROOT, preexec_fn=bounded, check=True)
        subprocess.run([sys.executable, 'tools/make_usb_image.py', '--output', str(image), '--update-boot'],
                       cwd=ROOT, preexec_fn=bounded, check=True)
        with image.open('rb') as source:
            assert validate_existing(source) == 128 * 2048
        assert digest(image)
    print('Capacity: 64 MiB creation, 1 GiB expansion/refresh under 384 MiB RAM, trailing-space growth, preservation and failure atomicity passed')


if __name__ == '__main__':
    main()
