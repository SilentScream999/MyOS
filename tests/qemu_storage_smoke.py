#!/usr/bin/env python3
"""Boot a disposable UEFI USB image in QEMU, write a file, cold boot, read it.

Run in WSL from the repo root after make all and tools/make_usb_image.py.
Requires qemu-system-x86_64, OVMF and nm. Output stays in .storage-test/.
"""
import json
import argparse
from pathlib import Path
import re
import socket
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / '.storage-test'
IMAGE = OUT / 'usb.img'
BOOT_ISO = False


class Qemu:
    def __init__(self, storage=True, resolution=None, hub_depth=0, extra_args=(), input_devices=True):
        command = [
            'qemu-system-x86_64', '-machine', 'q35', '-m', '512M',
            '-bios', '/usr/share/OVMF/OVMF_CODE.fd', '-display', 'none',
            '-device', 'qemu-xhci,id=xhci',
            '-qmp', 'tcp:127.0.0.1:45454,server=on,wait=off',
            '-no-reboot', '-rtc', 'base=localtime']
        hub_port = '1'
        if hub_depth:
            for index in range(hub_depth):
                command += ['-device', f'usb-hub,id=testhub{index},bus=xhci.0,port={hub_port}']
                hub_port += '.1'
            if input_devices:
                command += ['-device', 'usb-kbd,bus=xhci.0,port=2', '-device', 'usb-mouse,bus=xhci.0,port=3']
        elif input_devices:
            command += ['-device', 'usb-kbd', '-device', 'usb-mouse']
        if resolution:
            command += ['-vga', 'none', '-device', f'VGA,xres={resolution[0]},yres={resolution[1]}']
        if storage:
            command += ['-drive', f'if=none,id=stick,format=raw,file={IMAGE}',
                        '-device', f'usb-storage,drive=stick,bootindex={2 if BOOT_ISO else 1}' + (f',bus=xhci.0,port={hub_port}' if hub_depth else '')]
        if BOOT_ISO:
            command += ['-drive', f'if=none,id=bootcd,media=cdrom,readonly=on,file={ROOT / "wyzar.iso"}',
                        '-device', 'ide-cd,drive=bootcd,bootindex=1']
        command += list(extra_args)
        self.proc = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        for _ in range(100):
            try:
                self.sock = socket.create_connection(('127.0.0.1', 45454), timeout=2)
                break
            except OSError:
                if self.proc.poll() is not None:
                    raise RuntimeError(self.proc.stderr.read().decode())
                time.sleep(.1)
        else:
            raise RuntimeError('QMP unavailable')
        self.stream = self.sock.makefile('rwb')
        self.stream.readline()
        self.command('qmp_capabilities')

    def command(self, execute, arguments=None):
        msg = {'execute': execute}
        if arguments is not None:
            msg['arguments'] = arguments
        self.stream.write(json.dumps(msg).encode() + b'\n')
        self.stream.flush()
        while True:
            response = json.loads(self.stream.readline())
            if 'error' in response:
                raise RuntimeError(response)
            if 'return' in response:
                return response['return']

    def monitor(self, cmd):
        return self.command('human-monitor-command', {'command-line': cmd})

    def type(self, line, pause=2):
        codes = {' ': 'spc', '/': 'slash', '.': 'dot', '\n': 'ret', '-': 'minus', '_': 'shift-minus'}
        for char in line:
            result = self.monitor('sendkey ' + codes.get(char, char) + ' 40')
            assert not result, result
            time.sleep(.09)
        time.sleep(pause)

    def capture(self, label):
        self.command('screendump', {'filename': str(OUT / f'{label}.ppm')})
        symbols = subprocess.check_output(['nm', '-n', str(ROOT / 'wyzar')], text=True)
        address = re.search(r'^([0-9a-f]+) . g_klog_buffer$', symbols, re.M)[1]
        path = OUT / f'{label}.log'
        result = self.monitor(f'memsave 0x{address} 65536 "{path}"')
        assert not result, result
        return path.read_bytes().split(b'\0')[0].decode(errors='replace')

    def close(self):
        try:
            try:
                self.command('quit')
            except (ConnectionError, ValueError):
                pass # QEMU may close QMP before sending the quit response.
        finally:
            self.stream.close()
            self.sock.close()
            self.proc.wait(timeout=5)


def saved_file(shortname=b'HELLO   TXT', metadata=False):
    with IMAGE.open('rb') as disk:
        def read(lba):
            disk.seek(lba * 512)
            return disk.read(512)
        mbr = read(0)
        start = struct.unpack_from('<I', mbr, 446 + 16 + 8)[0]
        b = read(start)
        reserved, fats, fatsecs = struct.unpack_from('<H', b, 14)[0], b[16], struct.unpack_from('<H', b, 22)[0]
        roots = struct.unpack_from('<H', b, 17)[0]
        root = start + reserved + fats * fatsecs
        data = root + (roots * 32 + 511) // 512
        for index in range(roots):
            e = read(root + index // 16)[index % 16 * 32:index % 16 * 32 + 32]
            if e[:11] == shortname:
                if metadata:
                    return e
                cluster, size = struct.unpack_from('<HI', e, 26)
                return read(data + (cluster - 2) * b[13])[:size]
    return None


def main():
    global IMAGE, BOOT_ISO
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', action='store_true', help='Boot the current ISO with an attached persistent USB image, like make run')
    parser.add_argument('--image', type=Path, default=IMAGE)
    parser.add_argument('--features', action='store_true', help='Also verify timestamps, deletion, slot reuse, boot refresh and a third cold boot')
    args = parser.parse_args()
    IMAGE, BOOT_ISO = args.image.resolve(), args.iso
    initial_metadata = None
    for boot in ((1, 2, 3) if args.features else (1, 2)):
        q = Qemu()
        try:
            for attempt in range(90):
                time.sleep(2)
                try:
                    log = q.capture(f'boot{boot}')
                except AssertionError as error:
                    if 'Invalid addr' in str(error):
                        log = 'Kernel not mapped yet'
                        continue
                    raise
                if '[init] Decoupled terminal boot.' in log:
                    time.sleep(2)
                    break
            else:
                raise AssertionError('Boot did not complete: ' + log[-5000:])
            assert '[storage] WYZARDATA mounted as root /.' in log, log[-5000:]
            assert '[wallpaper] Loaded disk asset into RAM.' in log, log[-5000:]
            if boot == 1:
                q.type('write /hello.txt survived-cold-boot\n')
                if args.features:
                    q.type('write /gone.txt delete-me\n')
                    q.type('stat /hello.txt\n')
            elif args.features and boot == 2:
                q.type('cat /hello.txt\n')
                q.type('write /hello.txt updated-after-reboot\n')
                q.type('stat /hello.txt\n')
                updated = saved_file(metadata=True)
                assert updated[13:18] == initial_metadata[13:18], 'Creation time changed during edit'
                assert updated[22:26] != initial_metadata[22:26], 'Modification time did not advance'
                q.capture('timestamp-edit')
                q.type('rm /hello.txt\n')
                q.type('rm /gone.txt\n')
                q.type('write /reuse.txt reused-slot\n')
            elif args.features and boot == 3:
                q.type('cat /reuse.txt\n')
                q.type('stat /reuse.txt\n')
            else:
                q.type('cat /hello.txt\n')
                q.type('cat /readme.txt\n')
                q.type('cat /hello.txt\n')
                q.type('stat /hello.txt\n')
            q.capture(f'boot{boot}-shell')
        finally:
            q.close()
        if args.features and boot >= 2:
            assert saved_file() is None and saved_file(b'GONE    TXT') is None
            assert saved_file(b'REUSE   TXT') == b'reused-slot'
        else:
            assert saved_file() == b'survived-cold-boot', saved_file()
        if args.features and boot == 1:
            initial_metadata = saved_file(metadata=True)
            date = struct.unpack_from('<H', initial_metadata, 16)[0]
            assert 1980 + (date >> 9) >= 2026, 'Missing creation date'
            subprocess.run(['python3', str(ROOT / 'tools/make_usb_image.py'), '--output', str(IMAGE), '--update-boot'], check=True)
            assert saved_file() == b'survived-cold-boot'
        print(f'USB cold boot {boot}: mounted; persisted contents verified', flush=True)
    print('QEMU USB persistence smoke test passed. Screenshots/logs in .storage-test/.')


if __name__ == '__main__':
    main()
