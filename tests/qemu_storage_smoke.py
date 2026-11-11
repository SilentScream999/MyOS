#!/usr/bin/env python3
"""Shared QEMU harness; direct invocation runs the ext4 persistence suite.

Run in WSL from the repo root after make all and tools/make_usb_image.py.
Requires qemu-system-x86_64, OVMF and nm. Output stays in .storage-test/.
"""
import json
from pathlib import Path
import re
import socket
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


def main():
    # Keep the shared QEMU harness, and use the ext4 persistence/recovery suite.
    from qemu_ext4_test import main as ext4_main
    ext4_main()


if __name__ == '__main__':
    main()
