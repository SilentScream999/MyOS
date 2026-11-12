"""Real Start-menu discovery/launch on uppercase FAT names and RAM recovery."""
from pathlib import Path
import os
import subprocess
import time
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]
symbols = subprocess.check_output(['nm', '-n', str(ROOT / 'wyzar')], text=True)
def address(name):
    return next(int(line.split()[0], 16) for line in symbols.splitlines() if line.split()[-1].endswith(name))

def check(storage):
    smoke.IMAGE = ROOT / os.environ.get('MENU_TEST_IMAGE', '.storage-test/root.img')
    smoke.BOOT_ISO = not storage
    q = smoke.Qemu(storage=storage)
    def memory(name, count):
        path = ROOT / '.storage-test/menu-memory.bin'
        q.command('stop')
        try:
            assert not q.monitor(f'memsave 0x{address(name):x} {count} "{path}"')
            return path.read_bytes()
        finally:
            q.command('cont')
    def scalar(name, size=4, signed=False):
        return int.from_bytes(memory(name, size), 'little', signed=signed)
    def move(x, y):
        for _ in range(40):
            dx = x - scalar('g_mouse_x', signed=True)
            dy = y - scalar('g_mouse_y', signed=True)
            if abs(dx) <= 2 and abs(dy) <= 2:
                return
            q.monitor(f'mouse_move {max(-80,min(80,dx))} {max(-80,min(80,dy))}')
            time.sleep(.04)
        raise AssertionError('Mouse did not reach target')
    def click(x, y):
        move(x, y)
        q.monitor('mouse_button 1'); time.sleep(.08)
        q.monitor('mouse_button 0'); time.sleep(.3)
    try:
        for _ in range(40):
            time.sleep(1)
            try:
                log = q.capture('disk-menu-boot' if storage else 'recovery-menu-boot')
            except AssertionError:
                continue
            if '[wm] Shell task started inside WM frame.' in log:
                break
        else:
            raise AssertionError('Boot did not reach shell')
        click(45, 748)
        assert scalar('g_menu_open', 1)
        count = scalar('g_menu_app_count')
        names_raw = memory('g_menu_apps', 29 * count)
        names = [names_raw[i*29:(i+1)*29].split(b'\0')[0].decode() for i in range(count)]
        assert {name.lower() for name in names} == {'counter', 'win_test'}, names
        paths_raw = memory('g_menu_paths', 124 * count)
        paths = [paths_raw[i*124:(i+1)*124].split(b'\0')[0].decode() for i in range(count)]
        expected = {'/bin/COUNTER.ELF', '/bin/WIN_TEST.ELF'} if storage else {'/bin/counter.elf', '/bin/win_test.elf'}
        assert set(paths) == expected, paths
        index = next(i for i, name in enumerate(names) if name.lower() == 'counter')
        hdr, row, pad = max(24, 768*36//1080), max(26, 768*38//1080), max(6, 768*10//1080)
        top = 768 - scalar('g_taskbar_h') - (hdr + count*row + pad*2) - 768*4//1080
        click(80, top + hdr + pad + index*row + row//2)
        time.sleep(4)
        data = memory('g_term_matrix', 640000)[::8]
        text = '\n'.join(data[i:i+160].decode('latin1').rstrip('\0 ') for i in range(0,len(data),160)).rstrip()
        assert '[counter.elf] Done!' in text and text.endswith('wyzar>'), text
        assert not scalar('g_terminal_foreground_pid')
        q.capture('disk-menu-launched' if storage else 'recovery-menu-launched')
        # Both launch routes must finish normally when the title-bar X is clicked.
        baseline = scalar('g_window_count')
        terminal_focus = scalar('g_focused_win_id')
        for route in ('terminal', 'menu'):
            if route == 'terminal':
                q.type('run /bin/win_test.elf\n', pause=.5)
            else:
                click(45, 748)
                assert scalar('g_menu_open', 1)
                index = next(i for i, name in enumerate(names) if name.lower() == 'win_test')
                click(80, top + hdr + pad + index*row + row//2)
                time.sleep(.5)
            assert scalar('g_terminal_foreground_pid'), route
            assert scalar('g_window_count') == baseline + 1
            title_h = scalar('g_title_h')
            pad_x = max(2, 4*title_h//42)
            button = title_h - pad_x*2
            click(220 + 480 - pad_x - button//2, 160 - title_h + pad_x + button//2)
            for _ in range(20):
                if not scalar('g_terminal_foreground_pid'):
                    break
                time.sleep(.1)
            assert scalar('g_terminal_foreground_pid') == 0, route + ': process still running after close'
            assert scalar('g_window_count') == baseline, route + ': window not reclaimed'
            assert scalar('g_focused_win_id') == terminal_focus
            time.sleep(.2)
            data = memory('g_term_matrix', 640000)[::8]
            text = '\n'.join(data[i:i+160].decode('latin1').rstrip('\0 ') for i in range(0,len(data),160)).rstrip()
            assert text.endswith('wyzar>') and 'Program exited with code' not in text and '^C' not in text, text
        q.capture('disk-menu-closed' if storage else 'recovery-menu-closed')
        print(f'{"Disk" if storage else "Recovery"} Start menu passed: {names}, shell excluded, exact paths, counter returned; win_test close exits from terminal and menu', flush=True)
    finally:
        q.close()

if __name__ == '__main__':
    check(True)
    check(False)
