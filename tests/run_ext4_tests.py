#!/usr/bin/env python3
"""Run ext4 VFS, recovery and failure tests on disposable image copies."""
from pathlib import Path
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.storage-test'


def main():
    subprocess.run(['make', '-s', '.storage-test/ext4-test'], cwd=ROOT, check=True)
    original = OUT / 'ext4-usb.img'
    disk = OUT / 'ext4-test-run.img'
    shutil.copyfile(original, disk)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')
    for mode in ('normal', 'crash', 'recover', 'write-fail', 'flush-fail', 'offline'):
        subprocess.run([str(OUT / 'ext4-test'), str(disk), mode], cwd=ROOT, env=env, check=True)
        if mode in ('normal', 'recover'):
            partition = OUT / 'ext4-test-partition.img'
            with disk.open('rb') as source:
                source.seek((2048 + 262144) * 512)
                partition.write_bytes(source.read())
            subprocess.run(['e2fsck', '-fn', str(partition)], check=True, capture_output=True)
    print('ext4 sanitizer, cold recovery, failure containment and Linux checker tests passed')


if __name__ == '__main__':
    main()
