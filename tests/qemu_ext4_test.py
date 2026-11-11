#!/usr/bin/env python3
"""USB ext4 root, cold recovery, persistent saves and clean normal shutdown."""
from pathlib import Path
import argparse
import subprocess
import time
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parent.parent
IMAGE = ROOT / '.storage-test/ext4-usb.img'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hub-depth', type=int, choices=(0, 1, 2), default=0)
    args = parser.parse_args()
    smoke.IMAGE = IMAGE
    for boot in (1, 2, 3):
        q = smoke.Qemu(hub_depth=args.hub_depth)
        powered_off = False
        try:
            log = ''
            for attempt in range(60):
                time.sleep(2)
                try:
                    log = q.capture(f'ext4-boot{boot}')
                except AssertionError as error:
                    if 'Invalid addr' in str(error):
                        continue
                    raise
                if '[init] Decoupled terminal boot.' in log:
                    break
            else:
                raise AssertionError('ext4 boot failed: ' + log[-6000:])
            assert '[storage] ext4 journal active on USB system root.' in log, log[-6000:]
            assert '[storage] Discovery:' not in log, 'Failure snapshots must not appear on successful boots'
            assert '[storage] Waiting for USB system storage discovery.' not in log, 'An already-ready disk must skip the grace wait'
            assert '[wallpaper] Loaded disk asset into RAM.' in log, log[-6000:]
            assert 'milliseconds:' not in log and '[wallpaper] Asset bytes:' not in log, 'Extra timing logs must be off by default'
            if args.hub_depth:
                assert log.count('Hub child addressed successfully') >= args.hub_depth
            if boot == 1:
                q.type('write /home/replacement.txt old-long-content\n')
                q.type('write /home/replacement.txt ok\n')
                q.type('mkdir /home/documents\n')
                q.type('write /home/documents/long-filename.txt ext4-survived-cold-boot\n')
            else:
                q.type('cat /home/documents/long-filename.txt\n')
                q.type('df\n')
            q.capture(f'ext4-boot{boot}-shell')
            if boot == 3:
                q.type('shutdown\n', pause=.5)
                q.proc.wait(timeout=10)
                powered_off = True
        finally:
            if powered_off:
                q.stream.close(); q.sock.close()
            else:
                q.close()  # Intentional unclean poweroff; next boot must recover.
        with IMAGE.open('rb') as source:
            source.seek((2048 + 262144) * 512)
            partition = ROOT / '.storage-test/ext4-qemu-partition.img'
            partition.write_bytes(source.read())
        # Replay journal only on a COPY, then verify metadata and file contents.
        if boot < 3:
            check = subprocess.run(['e2fsck', '-p', str(partition)], capture_output=True)
            assert check.returncode in (0, 1), check.stdout + check.stderr
        else:
            state = int.from_bytes(partition.read_bytes()[1024+58:1024+60], 'little')
            assert state == 1, f'Normal shutdown left filesystem state {state}'
        subprocess.run(['e2fsck', '-fn', str(partition)], check=True, capture_output=True)
        content = subprocess.check_output(['debugfs', '-R', 'cat /home/documents/long-filename.txt', str(partition)], stderr=subprocess.DEVNULL)
        assert content == b'ext4-survived-cold-boot', content
        replacement = subprocess.check_output(['debugfs', '-R', 'cat /home/replacement.txt', str(partition)], stderr=subprocess.DEVNULL)
        assert replacement == b'ok', replacement
        print(f'ext4 USB boot {boot}: journal, wallpaper, persistence and Linux checker passed', flush=True)
    print('QEMU ext4 USB test passed')


if __name__ == '__main__':
    main()
