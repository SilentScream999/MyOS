#!/usr/bin/env python3
"""Shutdown/restart work with the system disk connected or hot-unplugged."""
from pathlib import Path
import argparse
import shutil
import subprocess
import time
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.storage-test'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--menu', action='store_true', help='Click Start power buttons instead of typing commands')
    args = parser.parse_args()
    symbols = subprocess.check_output(['nm', '-n', str(ROOT / 'wyzar')], text=True) if args.menu else ''
    def address(name):
        return int(next(line.split()[0] for line in symbols.splitlines() if line.split()[-1].endswith(name)), 16)
    smoke.BOOT_ISO = True
    for unplugged in (True, False):
        for action in ('shutdown', 'restart'):
            label = f'power-{action}-' + ('unplugged' if unplugged else 'connected')
            image = OUT / (label + '.img')
            shutil.copyfile(ROOT / 'wyzar-usb.img', image)
            q = smoke.Qemu(storage=False, resolution=(1280,800), hub_depth=1, extra_args=(
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
                if args.menu:
                    def scalar(name):
                        path = OUT / 'power-menu-memory.bin'
                        q.command('stop')
                        try:
                            assert not q.monitor(f'memsave 0x{address(name):x} 4 "{path}"')
                            return int.from_bytes(path.read_bytes(), 'little', signed=True)
                        finally:
                            q.command('cont')
                    def click(x, y, release=True):
                        for _ in range(80):
                            dx, dy = x-scalar('g_mouse_x'), y-scalar('g_mouse_y')
                            if abs(dx)<=1 and abs(dy)<=1:
                                break
                            q.monitor(f'mouse_move {max(-90,min(90,dx))} {max(-90,min(90,dy))}')
                            time.sleep(.025)
                        else:
                            raise AssertionError('Pointer did not reach power button')
                        q.monitor('mouse_button 1')
                        if release:
                            time.sleep(.1); q.monitor('mouse_button 0')
                    mouse = next(m for m in q.command('query-mice') if 'USB' in m['name'] or 'HID' in m['name'])
                    q.monitor(f"mouse_set {mouse['index']}")
                    bar = scalar('g_taskbar_h')
                    click(30, 800-bar//2)
                    time.sleep(.5)
                    q.command('screendump', {'filename': str(OUT / (label+'-menu.ppm'))})
                    width, pad = max(290,800*380//1080), max(10,800*14//1080)
                    button_width = (width-pad*3)//2
                    x = 6+pad+button_width//2+(button_width+pad if action=='shutdown' else 0)
                    y = 800-bar-800*4//1080-max(52,800*64//1080)//2
                    click(x, y, release=False)  # Guest can exit before a release event arrives.
                else:
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
