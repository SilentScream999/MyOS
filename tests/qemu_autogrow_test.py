#!/usr/bin/env python3
"""Boot an enlarged USB twice, then verify fixed DEBUG=1 on the same capacity."""
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]


def boot(label, setup):
    q = smoke.Qemu()
    try:
        for _ in range(120):
            time.sleep(2)
            try:
                log = q.capture(label)
            except AssertionError as error:
                if 'Invalid addr' in str(error):
                    continue  # Firmware has not entered the kernel yet.
                raise
            if '[init] Decoupled terminal boot.' in log:
                break
        else:
            raise AssertionError('Boot did not finish: ' + log[-6000:])
        assert '[storage] ext4 journal active on USB system root.' in log, log[-6000:]
        assert ('Creating expanded filesystem...' in log) == setup, log[-6000:]
        assert ('USB storage ready.' in log) == setup, log[-6000:]
        q.type('df\n')
        q.capture(label + '-df')
        q.type('shutdown\n', pause=.5)
        q.proc.wait(timeout=15)
        q.stream.close(); q.sock.close()
    finally:
        if q.proc.poll() is None:
            q.close()


def main():
    with tempfile.TemporaryDirectory(prefix='wyzar-qemu-autogrow-') as directory:
        disk = Path(directory) / 'usb.img'
        smoke.IMAGE = disk
        for debug in (False, True):
            command = ['make', '-s', 'usb-image', 'FRESH=1', f'PERSIST_DISK={disk}']
            if debug:
                command.append('DEBUG=1')
            subprocess.run(command, cwd=ROOT, check=True)
            assert disk.stat().st_size == 385 * 1024 * 1024
            with disk.open('r+b') as image:
                image.truncate(1024 * 1024 * 1024)
            for count in (1, 2):
                boot(f'autogrow-{"debug" if debug else "normal"}-{count}', not debug and count == 1)
                with disk.open('rb') as image:
                    image.seek(470); start, sectors = struct.unpack('<II', image.read(8))
                assert sectors == (524288 if debug else 639 * 2048)
                volume = Path(directory) / 'root.img'
                with disk.open('rb') as image, volume.open('wb') as output:
                    image.seek(start * 512)
                    for _ in range(sectors // 2048):
                        output.write(image.read(1024 * 1024))
                subprocess.run(['e2fsck', '-fn', str(volume)], check=True, capture_output=True)
                print(f'{"Debug" if debug else "Automatic"} boot {count}: root {sectors // 2048} MiB, clean shutdown/checker passed', flush=True)
    print('QEMU automatic first boot, repeat boot and fixed DEBUG=1 passed')


if __name__ == '__main__':
    main()
