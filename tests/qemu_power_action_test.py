#!/usr/bin/env python3
"""Shutdown/restart work with the system disk connected or hot-unplugged."""
from pathlib import Path
import shutil
import subprocess
import time
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.storage-test'


def main():
    smoke.BOOT_ISO = True
    for unplugged in (True, False):
        for action in ('shutdown', 'restart'):
            label = f'power-{action}-' + ('unplugged' if unplugged else 'connected')
            image = OUT / (label + '.img')
            shutil.copyfile(ROOT / 'wyzar-usb.img', image)
            q = smoke.Qemu(storage=False, hub_depth=1, extra_args=(
                '-drive', f'if=none,id=powerdisk,format=raw,file={image}',
                '-device', 'usb-storage,id=system-stick,drive=powerdisk,bus=xhci.0,port=1.1'))
            finished = False
            try:
                log = ''
                for _ in range(60):
                    time.sleep(1)
                    try:
                        log = q.capture(label)
                    except AssertionError as error:
                        if 'Invalid addr' in str(error):
                            continue
                        raise
                    if '[init] Decoupled terminal boot.' in log:
                        break
                else:
                    raise AssertionError(log[-5000:])
                assert '[storage] ext4 journal active on USB system root.' in log
                q.type('write /home/power-test.txt persisted-before-power-action\n')
                if unplugged:
                    q.command('device_del', {'id': 'system-stick'})
                    for _ in range(50):
                        time.sleep(.1)
                        devices = q.command('qom-list', {'path': '/machine/peripheral'})
                        if not any(d['name'] == 'system-stick' for d in devices):
                            break
                    else:
                        raise AssertionError('USB device removal did not finish')
                    time.sleep(2)  # Let the guest's hub monitor observe removal.
                q.type(action + '\n', pause=.1)
                # -no-reboot makes a guest reset exit QEMU, just like poweroff.
                # Never issue QMP quit here: the guest must perform the action.
                assert q.proc.wait(timeout=10) == 0
                finished = True
            finally:
                if finished:
                    q.stream.close()
                    q.sock.close()
                else:
                    q.close()
            partition = OUT / (label + '-partition.img')
            with image.open('rb') as disk:
                disk.seek((2048 + 262144) * 512)
                partition.write_bytes(disk.read())
            if not unplugged:
                state = int.from_bytes(partition.read_bytes()[1082:1084], 'little')
                assert state == 1, f'Connected {action} left filesystem state {state}'
            else:
                check = subprocess.run(['e2fsck', '-p', str(partition)], capture_output=True)
                assert check.returncode in (0, 1), check.stdout + check.stderr
            subprocess.run(['e2fsck', '-fn', str(partition)], check=True, capture_output=True)
            data = subprocess.check_output(['debugfs', '-R', 'cat /home/power-test.txt', str(partition)],
                                           stderr=subprocess.DEVNULL)
            assert data == b'persisted-before-power-action', data
            print(label + ': guest power action, filesystem check and saved data passed', flush=True)
            # Each case uses disposable copies; do not accumulate multi-GB data.
            partition.unlink()
            image.unlink()


if __name__ == '__main__':
    main()
