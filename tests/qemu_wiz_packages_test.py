#!/usr/bin/env python3
"""Run after make all and creating .storage-test/ext4-usb.img.

Boot a disposable copy and exercise the real package shell, loader, EXEPATH,
EXCHANGE and saved-data syscalls. Requires the shared QEMU harness dependencies.
"""
from pathlib import Path
import re
import shutil
import subprocess
import time
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.storage-test'


def main():
    smoke.IMAGE = OUT / 'wiz-packages.img'
    shutil.copyfile(OUT / 'ext4-usb.img', smoke.IMAGE)
    symbols = subprocess.check_output(['nm', '-n', str(ROOT / 'wyzar')], text=True)
    address = re.search(r'^([0-9a-f]+) . _ZL13g_term_matrix$', symbols, re.M)[1]
    q = smoke.Qemu()

    def terminal():
        path = OUT / 'wiz-packages.matrix'
        q.command('stop')
        try:
            assert not q.monitor(f'memsave 0x{address} 640000 "{path}"')
            data = path.read_bytes()[::8]
            return '\n'.join(data[i:i+160].decode('latin1').rstrip('\0 ')
                             for i in range(0, len(data), 160)).rstrip()
        finally:
            q.command('cont')

    def command(line, expected):
        q.type('clear\n', pause=.2)
        q.type(line + '\n', pause=1)
        text = terminal()
        assert expected in text, text
        print(f'guest passed: {line}', flush=True)

    try:
        log = ''
        for attempt in range(60):
            time.sleep(1)
            try:
                log = q.capture('wiz-packages-boot')
            except AssertionError as error:
                if 'Invalid addr' in str(error):
                    continue
                raise
            if '[init] Decoupled terminal boot.' in log:
                break
        else:
            raise AssertionError('package test failed to boot: ' + log[-6000:])
        assert '[storage] ext4 journal active on USB system root.' in log, log[-6000:]
        # The shell starts after the init diagnostic is emitted.
        time.sleep(2)
        command('run /share/samples/bundle_demo.wiz', 'This is run number 1')
        command('wiz install /share/samples/bundle_demo.wiz', 'installed Bundle Demo')
        command('wiz install /share/samples/bundle_demo.wiz', 'reinstalled Bundle Demo')
        command('wiz info bundle-demo', 'checksum ok')
        command('run /apps/bundle-demo/bundle-demo.wiz', 'This is run number 2')
        command('wiz remove bundle-demo', 'kept its data')
        command('wiz pin /share/samples/bundle_demo.wiz', 'pinned Bundle Demo')
        command('wiz remove --purge bundle-demo', 'deleted its data')
        command('run /share/samples/bundle_demo.wiz', 'This is run number 1')
        q.capture('wiz-packages-complete')
        print('QEMU package lifecycle and real syscall integration passed', flush=True)
    finally:
        q.close()


if __name__ == '__main__':
    main()
