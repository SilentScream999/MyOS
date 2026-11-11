#!/usr/bin/env python3
"""Measure boot asset timings on a disposable copy of the current USB image."""
import argparse
from pathlib import Path
import re
import shutil
import time
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--label', default='wallpaper-perf')
    parser.add_argument('--resolution', default='1920x1080')
    parser.add_argument('--image', type=Path, default=ROOT / 'wyzar-usb.img')
    parser.add_argument('--expect-fallback', action='store_true')
    args = parser.parse_args()
    assert re.fullmatch(r'[a-zA-Z0-9_-]+', args.label)
    width, height = map(int, args.resolution.split('x'))
    smoke.IMAGE = ROOT / '.storage-test/wallpaper-perf.img'
    smoke.OUT.mkdir(exist_ok=True)
    shutil.copyfile(args.image, smoke.IMAGE)
    q = smoke.Qemu(resolution=(width, height))
    try:
        log = ''
        for attempt in range(90):
            time.sleep(1)
            try:
                log = q.capture(args.label)
            except AssertionError as error:
                if 'Invalid addr' in str(error):
                    continue
                raise
            if '[init] Decoupled terminal boot.' in log:
                break
        else:
            raise AssertionError(log[-6000:])
        if args.expect_fallback:
            assert '[wallpaper] Missing, unreadable or invalid asset; keeping fallback.' in log, log[-6000:]
        else:
            assert '[wallpaper] Loaded disk asset into RAM.' in log, log[-6000:]
            assert '[wallpaper] Fallback generation milliseconds:' not in log
        for line in log.splitlines():
            if 'milliseconds:' in line:
                match = re.search(re.escape(line) + r'\s+(\d+)', log)
                print(line, match[1] if match else '?', flush=True)
        if 'milliseconds:' not in log:
            print('Boot passed; extra timing logs disabled (BOOT_DIAGNOSTICS=0).', flush=True)
    finally:
        q.close()


if __name__ == '__main__':
    main()
