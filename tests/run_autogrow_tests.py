#!/usr/bin/env python3
"""Native first-boot setup, fixed debug policy, retry and Linux validation."""
from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from image_io import copy_sparse
from make_usb_image import storage_policy, START, BOOT_SECTORS


def main():
    subprocess.run(['make', '-s', '.storage-test/autogrow-test', '.storage-test/ext4-image-tool'], cwd=ROOT, check=True)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')
    start = START + BOOT_SECTORS
    with tempfile.TemporaryDirectory(prefix='wyzar-autogrow-') as directory:
        work = Path(directory); seed = work / 'seed.img'; disk = work / 'disk.img'; volume = work / 'volume.img'
        subprocess.run([sys.executable, 'tools/make_usb_image.py', '--output', str(seed), '--fresh'], cwd=ROOT, check=True)
        with seed.open('rb') as source, volume.open('wb') as destination:
            source.seek(start * 512); copy_sparse(source, destination, 256 * 1024 * 1024)
        data = work / 'asset'; data.write_bytes(b'seed-user-data!!')
        manifest = work / 'manifest'; manifest.write_text(f'put /home/preserved.txt {data}\n')
        subprocess.run([str(ROOT / '.storage-test/ext4-image-tool'), str(volume), str(manifest)], check=True)
        with volume.open('rb') as source, seed.open('r+b') as destination:
            destination.seek(start * 512); copy_sparse(source, destination, volume.stat().st_size)
        with seed.open('rb') as source:
            source.seek(start * 512 + 1128); uuid = source.read(16)

        def reset(capacity=1024, fixed=False):
            with seed.open('rb') as source, disk.open('wb') as destination:
                copy_sparse(source, destination, seed.stat().st_size)
                destination.truncate(capacity * 1024 * 1024)
                if fixed:
                    destination.seek(512); destination.write(storage_policy(uuid, 524288, False))

        def run(expect, budget=None):
            command = [str(ROOT / '.storage-test/autogrow-test'), str(disk), expect]
            if budget is not None: command.append(str(budget))
            subprocess.run(command, env=env, cwd=ROOT, check=True, timeout=120)

        def check_partition(offset, sectors):
            with disk.open('rb') as source, volume.open('wb') as destination:
                source.seek(offset * 512); copy_sparse(source, destination, sectors * 512)
            subprocess.run(['e2fsck', '-fn', str(volume)], check=True, capture_output=True)
            result = subprocess.check_output(['debugfs', '-R', 'cat /home/preserved.txt', str(volume)], stderr=subprocess.DEVNULL)
            assert result == b'seed-user-data!!'

        reset(); run('grow')
        with disk.open('rb') as source:
            source.seek(470); offset, sectors = struct.unpack('<II', source.read(8))
        assert offset == start + 524288
        check_partition(offset, sectors)
        check_partition(start, 524288)  # The recovery seed is still intact/clean.
        run('skip')  # A separate process models the next cold boot.
        reset(fixed=True); run('skip'); check_partition(start, 524288)
        reset(capacity=512); run('skip')
        for budget in (-3, -4):
            reset(); run('uncertain', budget); check_partition(start, 524288)
            run('grow' if budget == -3 else 'skip')
        for budget in (0, 5, 40, 300, 2500, -2):
            reset(); run('fail', budget); check_partition(start, 524288)
            run('grow')  # Reboot/retry after interrupted setup.
        reset()
        with disk.open('r+b') as destination:
            destination.seek(512 + 100); destination.write(b'broken')
        run('skip')  # Invalid policy never authorizes formatting.
        reset()
        with disk.open('r+b') as destination:
            destination.seek(start * 512 + 1128); destination.write(b'wrong-root-uuid!')
        run('fail')  # A policy cannot authorize another seed volume.
        reset()
        with disk.open('r+b') as destination:
            destination.seek(start * 512 + 1120); destination.write(struct.pack('<I', 0x80000000))
        run('fail')  # Unsupported features are rejected before any seed mount.
        # Formatter geometry: multiple descriptor blocks and a discarded tiny
        # final block group. Linux must accept both, including free counts.
        for size in (32768 * 1024 * 1024, 384 * 1024 * 1024 + 4096):
            with volume.open('wb') as destination: destination.truncate(size)
            subprocess.run([str(ROOT / '.storage-test/autogrow-test'), str(volume), 'format'], env=env, check=True, timeout=120)
            subprocess.run(['e2fsck', '-fn', str(volume)], check=True, capture_output=True)
    print('Autogrow: native provisioning, preserved seed, fixed debug, small media, interrupted retries and invalid policy passed')


if __name__ == '__main__':
    main()
