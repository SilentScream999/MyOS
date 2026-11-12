#!/usr/bin/env python3
"""QEMU checks for Ctrl+C and real mouse-driven Start-menu foreground launches."""
import struct
import subprocess
import time
import qemu_storage_smoke as smoke
from qemu_shell_wait import build_probes, ROOT, OUT


def main():
    smoke.IMAGE = OUT / 'shell-wait.img'
    smoke.BOOT_ISO = True
    build_probes()
    symbols = subprocess.check_output(['nm', '-n', str(ROOT / 'wyzar')], text=True)
    def address(name):
        return next(int(line.split()[0], 16) for line in symbols.splitlines() if line.split()[-1].endswith(name))
    q = smoke.Qemu()
    def memory(name, count):
        path = OUT / 'ctrlc-memory.bin'
        q.command('stop')
        try:
            assert not q.monitor(f'memsave 0x{address(name):x} {count} "{path}"')
            return path.read_bytes()
        finally:
            q.command('cont')
    def scalar(name, size=4, signed=False):
        return int.from_bytes(memory(name, size), 'little', signed=signed)
    def text(label):
        data = memory('g_term_matrix', 640000)[::8]
        result = '\n'.join(data[i:i+160].decode('latin1').rstrip('\0 ') for i in range(0, len(data), 160)).rstrip()
        (OUT / (label + '.txt')).write_text(result)
        return result
    def ctrlc():
        q.monitor('sendkey ctrl-c 60')
        time.sleep(.7)
        for _ in range(10):
            if not scalar('g_terminal_foreground_pid'):
                break
            time.sleep(.15)
        assert scalar('g_terminal_foreground_pid') == 0
        result = text('ctrlc-prompt')
        assert result.rstrip().endswith('wyzar>') and '^C' in result, result
    def move(x, y):
        for _ in range(32):
            current_x = scalar('g_mouse_x', signed=True)
            current_y = scalar('g_mouse_y', signed=True)
            dx, dy = x - current_x, y - current_y
            if abs(dx) <= 2 and abs(dy) <= 2:
                return
            q.monitor(f'mouse_move {max(-80, min(80, dx))} {max(-80, min(80, dy))}')
            time.sleep(.04)
        raise AssertionError('Mouse did not reach target')
    def click(x, y):
        move(x, y)
        q.monitor('mouse_button 1'); time.sleep(.08)
        q.monitor('mouse_button 0'); time.sleep(.2)
    def menu_launch(name):
        click(45, 748)
        assert scalar('g_menu_open', 1)
        count = scalar('g_menu_app_count')
        names = [part[0].split(b'\0')[0].decode() for part in struct.iter_unpack('29s', memory('g_menu_apps', 29 * count))]
        index = names.index(name)
        hdr, row, pad = max(24, 768 * 36 // 1080), max(26, 768 * 38 // 1080), max(6, 768 * 10 // 1080)
        top = 768 - scalar('g_taskbar_h') - (hdr + count * row + pad * 2) - 768 * 4 // 1080
        click(80, top + hdr + pad + index * row + row // 2)
    try:
        time.sleep(22)
        shell_focus = scalar('g_focused_win_id')
        for program in ('input', 'busy'):
            q.type(f'run /disk/{program}.elf\n', pause=.3)
            assert scalar('g_terminal_foreground_pid')
            ctrlc()
        q.type('rm /disk/lock.txt\n', pause=.3)
        assert 'File deleted.' in text('ctrlc-file-cleanup')
        q.type('run /bin/counter.elf\n', pause=0)
        q.monitor('sendkey ctrl-c 60'); time.sleep(.6)
        assert not scalar('g_terminal_foreground_pid')
        # Menu request replaces a partially edited command and wakes stdin.
        q.type('unfinished', pause=0)
        baseline = scalar('g_window_count')
        menu_launch('win_test')
        time.sleep(.5)
        assert scalar('g_terminal_foreground_pid') and scalar('g_window_count') == baseline + 1
        launched = text('menu-gui-launched')
        assert 'wyzar> run /bin/win_test.elf' in launched and not launched.rstrip().endswith('wyzar>'), launched
        click(300, 200) # focus the user GUI, then Ctrl+C must still work
        assert scalar('g_focused_win_id') != shell_focus
        ctrlc()
        assert scalar('g_window_count') == baseline and scalar('g_focused_win_id') == shell_focus
        menu_launch('counter')
        time.sleep(3)
        counter = text('menu-counter-finished')
        assert 'run /bin/counter.elf' in counter and counter.rstrip().endswith('wyzar>'), counter
        assert scalar('g_terminal_foreground_pid') == 0
        q.capture('ctrlc-menu-complete')
        print('Ctrl+C/menu tests passed: input, CPU loop, sleeping counter, FD cleanup, partial-line menu launch, focused GUI cancellation/window cleanup and menu completion')
    finally:
        q.close()


if __name__ == '__main__':
    main()
