#!/usr/bin/env python3
"""Run after make iso PERSIST_DISK=.storage-test/shell-wait.img.
Checks actual terminal text via QMP, without requiring a key to restore a prompt.
Only the disposable .storage-test image is modified.
"""
import importlib.util
from pathlib import Path
import re
import subprocess
import time
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / '.storage-test'


def build_probes():
    for name, extra in (('exit', ['-DEXIT_ONLY']), ('input', ['-DREAD_ONLY']),
                        ('echo', ['-DREAD_ONLY', '-DENABLE_TEST_ECHO']), ('ignore', ['-DIGNORE_INPUT']),
                        ('busy', ['-DBUSY_LOOP']), ('wait', [])):
        obj = OUT / (name + '.o')
        elf = OUT / (name + '.elf')
        subprocess.run(['clang', '-ffreestanding', '-fno-stack-protector', '-fno-pic', '-fno-pie',
                        '-mno-red-zone', '-target', 'x86_64-elf', '-fno-builtin', *extra,
                        '-c', str(ROOT / 'tests/wait_probe.c'), '-o', str(obj)], check=True)
        subprocess.run(['ld.lld', '-nostdlib', '-z', 'max-page-size=0x1000', '-Ttext=0x400000',
                        '-o', str(elf), str(obj)], check=True)
    spec = importlib.util.spec_from_file_location('image_builder', ROOT / 'tools/make_usb_image.py')
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    data = builder.Fat16Image(builder.DATA_SECTORS, 'WYZARDATA', builder.START + builder.BOOT_SECTORS)
    for name in ('exit', 'input', 'echo', 'ignore', 'busy', 'wait'):
        data.add(name.upper() + '.ELF', (OUT / (name + '.elf')).read_bytes())
    with smoke.IMAGE.open('r+b') as image:
        image.seek((builder.START + builder.BOOT_SECTORS) * 512)
        image.write(data.data)


def main():
    smoke.IMAGE = OUT / 'shell-wait.img'
    smoke.BOOT_ISO = True
    build_probes()
    symbols = subprocess.check_output(['nm', '-n', str(ROOT / 'wyzar')], text=True)
    address = re.search(r'^([0-9a-f]+) . _ZL13g_term_matrix$', symbols, re.M)[1]
    q = smoke.Qemu()
    def terminal(label):
        path = OUT / (label + '.matrix')
        # Freeze the guest before disk-backed QMP dumps: writing the dump on
        # /mnt/c can take longer than the counter's whole 2.5-second runtime.
        q.command('stop')
        try:
            assert not q.monitor(f'memsave 0x{address} 640000 "{path}"')
            data = path.read_bytes()[::8] # TermCell is uint8_t + padding + uint32_t
            text = '\n'.join(data[i:i+160].decode('latin1').rstrip('\0 ') for i in range(0, len(data), 160)).rstrip()
            (OUT / (label + '.txt')).write_text(text)
            return text
        finally:
            q.command('cont')
    try:
        time.sleep(22)
        # A child blocked on input makes this independent of emulated timer
        # speed and host screenshot latency. It cannot exit before Return.
        q.type('run /disk/input.elf\n', pause=.2)
        first = terminal('child-running')
        assert '[input-probe] running' in first, first
        assert 'wyzar>' not in first.split('[input-probe] running', 1)[1], first
        q.type('x', pause=0)
        time.sleep(.15)
        during = terminal('child-key-during-run')
        assert 'wyzar>' not in during.split('[input-probe] running', 1)[1], during
        assert during == first, 'Foreground input was echoed despite echo being disabled'
        q.type('\n', pause=.3)
        child_done = terminal('child-finished')
        assert '[input-probe] finished' in child_done and child_done.rstrip().endswith('wyzar>'), child_done
        q.type('run /disk/echo.elf\n', pause=.2)
        q.type('echoed', pause=.2)
        echo = terminal('child-opt-in-echo')
        assert echo.rstrip().endswith('echoed'), echo
        q.type('\n', pause=.3)
        echo_done = terminal('child-echo-finished')
        assert echo_done.rstrip().endswith('wyzar>'), echo_done
        q.type('run /disk/ignore.elf\n', pause=.2)
        ignored_start = terminal('child-ignoring-input')
        assert ignored_start.rstrip().endswith('[ignore-probe] running'), ignored_start
        q.type('z\n', pause=3.5)
        ignored_done = terminal('child-input-discarded')
        tail = ignored_done.rsplit('[ignore-probe] running', 1)[1]
        assert tail.strip() == '[ignore-probe] finished\n\nwyzar>', tail
        q.type('run /bin/counter.elf\n', pause=3)
        done = terminal('counter-finished')
        assert 'Done! Exiting cleanly' in done, done
        assert done.split('Done! Exiting cleanly', 1)[1].rstrip().endswith('wyzar>'), done
        q.capture('counter-foreground')
        q.type('run /disk/wait.elf\n', pause=1)
        for _ in range(10):
            probe = terminal('wait-probe')
            if '[wait-probe] passed' in probe or '[wait-probe] FAILED' in probe:
                break
            time.sleep(.3)
        q.capture('wait-probe-state')
        assert '[wait-probe] passed fast exit, exit status, repeated and nonchild waits' in probe, probe
        assert probe.rstrip().endswith('wyzar>'), probe
        q.type('run /does-not-exist.elf\n', pause=.5)
        failed = terminal('spawn-failure')
        assert 'Failed to spawn process' in failed and failed.rstrip().endswith('wyzar>'), failed
        q.capture('shell-wait-complete')
        print('Shell tests passed: foreground echo off, program opt-in echo, leftover input discarded, prompt restoration, counter, wait/exit and spawn failure')
    finally:
        q.close()


if __name__ == '__main__':
    main()
