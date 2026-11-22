#!/usr/bin/env python3
"""Reused media must never replay transactions from an earlier filesystem."""
from pathlib import Path
import os
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    subprocess.run(['make', '-s', '.storage-test/journal-reformat-test'], cwd=ROOT, check=True)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')
    binary = ROOT / '.storage-test/journal-reformat-test'
    with tempfile.TemporaryDirectory(prefix='wyzar-journal-reformat-') as directory:
        volume = Path(directory) / 'root.img'
        for mib in (64, 384):
            with volume.open('wb') as image:
                image.truncate(mib * 1024 * 1024)
            # Formatting an already journaled volume must invalidate records
            # even when the next boot performs no user-initiated writes.
            for generation in range(2):
                subprocess.run([str(binary), str(volume), 'old'], cwd=ROOT,
                               env=env, check=True, timeout=120)
                journal = subprocess.check_output(
                    ['debugfs', '-R', 'cat <8>', str(volume)], stderr=subprocess.DEVNULL)
                sequence, start = struct.unpack_from('>II', journal, 24)
                assert sequence > 0 and start == 0, (sequence, start)
                for mode in ('reformat', 'boot', *(['recover'] * 8), 'verify'):
                    subprocess.run([str(binary), str(volume), mode], cwd=ROOT,
                                   env=env, check=True, timeout=120)
                subprocess.run(['e2fsck', '-fn', str(volume)], check=True, capture_output=True)
                print(f'{mib} MiB generation {generation + 1}: repeated abrupt boots, '
                      'marker/document integrity and filesystem check passed', flush=True)


if __name__ == '__main__':
    main()
