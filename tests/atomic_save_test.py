#!/usr/bin/env python3
"""Fail writes at publication/cleanup points; destination must stay old or new."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.storage-test'


def main():
    subprocess.run(['make', '-s', '.storage-test/ext4-test'], cwd=ROOT, check=True)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')
    with tempfile.TemporaryDirectory(prefix='atomic-save-', dir=OUT) as directory:
        work = Path(directory)
        disk = work / 'disk.img'
        partition = work / 'partition.img'
        outcomes = set()
        for budget in (0, 1, 7, 8, 15, 24, 48, 80, 120, 160, 240, 400, 800, 1600):
            shutil.copyfile(ROOT / 'wyzar-usb.img', disk)
            subprocess.run([str(OUT / 'ext4-test'), str(disk), 'atomic-fault', str(budget)],
                           cwd=ROOT, env=env, check=True)
            with disk.open('rb') as source:
                source.seek((2048 + 262144) * 512)
                partition.write_bytes(source.read())
            result = subprocess.run(['e2fsck', '-p', str(partition)], capture_output=True)
            assert result.returncode in (0, 1), (budget, result.stdout, result.stderr)
            subprocess.run(['e2fsck', '-fn', str(partition)], check=True, capture_output=True)
            data = subprocess.check_output(['debugfs', '-R', 'cat /home/atomic.txt', str(partition)],
                                           stderr=subprocess.DEVNULL)
            assert data in (b'old-complete', b'N' * 8193), (budget, len(data), data[:50])
            outcomes.add('old' if data == b'old-complete' else 'new')
        assert outcomes == {'old', 'new'}, outcomes
    print('Atomic save: 14 write-failure points recovered to a complete old/new destination')


if __name__ == '__main__':
    main()
