#!/usr/bin/env python3
"""Run after make iso. Exercise independent terminals using the RAM recovery root."""
from pathlib import Path
import re
import struct
import subprocess
import time
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]
OUT = smoke.OUT


def main():
    OUT.mkdir(exist_ok=True)
    symbols = subprocess.check_output(['nm', '-n', '-C', str(ROOT / 'wyzar')], text=True).splitlines()

    def address(name):
        return next(int(line.split()[0], 16) for line in symbols if line.split()[-1] == name)

    font = (ROOT / 'src/gui/font8x8_basic.h').read_text()
    glyphs = re.findall(r'\{((?:\s*0x[0-9A-Fa-f]+,?){8})\s*\}', font)
    characters = {tuple(int(n, 16) for n in re.findall(r'0x[0-9A-Fa-f]+', glyph)): chr(i)
                  for i, glyph in enumerate(glyphs) if 32 <= i < 127}
    assert len(characters) > 80
    smoke.BOOT_ISO = True
    q = smoke.Qemu(storage=False, resolution=(1280, 800))
    q.sock.settimeout(30)

    def memory(addr, size):
        path = OUT / 'terminal-state.bin'
        q.command('stop')
        try:
            assert not q.monitor(f'memsave 0x{addr:x} {size} "{path}"')
            return path.read_bytes()
        finally:
            q.command('cont')

    def scalar(name):
        return int.from_bytes(memory(address(name), 4), 'little')

    def terminals():
        count = scalar('g_window_count')
        if not count:
            return []
        result = []
        for (pointer,) in struct.iter_unpack('<Q', memory(address('g_window_list'), count * 8)):
            window = memory(pointer, 384)
            if window[4:68].split(b'\0')[0] == b'Terminal':
                result.append((pointer, struct.unpack_from('<iiii', window, 324)))
        return result

    def text(window):
        pointer, (_, _, width, height) = window
        buffer = struct.unpack_from('<Q', memory(pointer, 384), 344)[0]
        pixels = memory(buffer, width * height * 4)
        lines = []
        for y in range(height // 10):
            line = ''
            for x in range(width // 8):
                glyph = tuple(sum((struct.unpack_from('<I', pixels, ((y * 10 + gy) * width + x * 8 + gx) * 4)[0]
                                   != 0xff0a0a14) << gx for gx in range(8)) for gy in range(8))
                line += characters.get(glyph, '?')
            lines.append(line.rstrip())
        return '\n'.join(lines).rstrip()

    def wait_for(check):
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            try:
                if check():
                    return
            except AssertionError:
                pass  # Kernel symbols become readable after the firmware handoff.
            time.sleep(.3)
        raise AssertionError('Terminal condition timed out')

    def click(x, y):
        for _ in range(100):
            dx, dy = x - scalar('g_mouse_x'), y - scalar('g_mouse_y')
            if abs(dx) <= 1 and abs(dy) <= 1:
                break
            q.monitor(f'mouse_move {max(-80, min(80, dx))} {max(-80, min(80, dy))}')
            time.sleep(.025)
        q.monitor('mouse_button 1')
        time.sleep(.1)
        q.monitor('mouse_button 0')
        time.sleep(.4)

    def close(window):
        _, (x, y, width, _) = window
        title = scalar('g_title_h')
        click(x + width - max(2, title // 10) - (title + 8) // 2, y - title // 2)

    try:
        time.sleep(22)
        wait_for(lambda: len(terminals()) == 1)
        first = terminals()[0]
        wait_for(lambda: 'Welcome to WyzarOS' in text(first))
        print('Initial userspace terminal ready.', flush=True)
        q.type('write /tmp/one first\n')
        q.type('cat /tmp/one\n')
        assert 'first' in text(first)
        for _ in range(4):
            q.type('help\n', pause=.2)
        live = text(first)
        q.monitor('sendkey shift-up 100')
        time.sleep(.5)
        assert text(first) != live, 'Shift+Up did not reveal history'
        q.monitor('sendkey shift-down 100')
        time.sleep(.5)
        assert text(first) == live, 'Shift+Down did not return to the live screen'
        q.monitor('sendkey ctrl-alt-t 100')
        wait_for(lambda: len(terminals()) == 2)
        second = terminals()[1]
        wait_for(lambda: 'Welcome to WyzarOS' in text(second))
        assert 'Available commands:' not in text(second), 'Terminal output leaked between channels'
        q.type('cd /tmp\n')
        assert text(second).endswith('wyzar:/tmp>')
        assert text(first) == live, 'Typing in the second terminal changed the first'
        close(second)
        wait_for(lambda: len(terminals()) == 1)
        close(first)
        wait_for(lambda: len(terminals()) == 0)
        q.monitor('sendkey ctrl-alt-t 100')
        wait_for(lambda: len(terminals()) == 1)
        reopened = terminals()[0]
        q.type('pwd\n')
        assert text(reopened).endswith('wyzar:/>')
        usage = next(int(line.split()[0], 16) for line in symbols if line.endswith('::usage'))

        def cpu_samples():
            result = []
            for _ in range(6):
                time.sleep(.5)
                result.append(struct.unpack_from('<I', memory(usage, 24), 16)[0])
            return result

        baseline = cpu_samples()
        for count in range(2, 16):
            q.monitor('sendkey ctrl-alt-t 80')
            wait_for(lambda: len(terminals()) == count)
        time.sleep(2)
        many = cpu_samples()
        print(f'Idle HUD CPU with 1 terminal {baseline}, with 15 terminals {many}.', flush=True)
        assert sum(many) / len(many) <= sum(baseline) / len(baseline) + 5, 'Idle terminal CPU grew excessively'
        buffer = int.from_bytes(memory(address('g_master_backbuffer'), 8), 'little')
        bar_height = scalar('g_taskbar_h')
        edge = int.from_bytes(memory(buffer + ((800 - bar_height + 5) * 1280 + 1273) * 4, 4), 'little')
        assert edge == 0xff303560, 'Task buttons overwrote the clock tray'
        start = 6 + scalar('g_sb_w') + 10 * 800 // 1080
        width = scalar('g_wbtn_w')
        stride = width + 4 * 800 // 1080

        def pick_terminal():
            click(start + 3 * stride + width // 2, 800 - bar_height // 2)
            assert scalar('g_taskbar_popup') == 2, 'Terminal group did not open its window picker'
            q.command('screendump', {'filename': str(OUT / 'taskbar-window-picker.ppm')})
            panel_y = 800 - bar_height - 40 - len(terminals()) * 32 - 4
            click(start + 3 * stride + 70, panel_y + 52)
            assert scalar('g_taskbar_popup') == 0
            assert scalar('g_focused_win_id') == int.from_bytes(memory(reopened[0], 4), 'little')

        pick_terminal()
        q.type('pwd\n')
        assert text(reopened).endswith('wyzar:/>')
        for app in ('explorer', 'notepad', 'image_viewer', 'task_manager'):
            pick_terminal()
            before = scalar('g_window_count')
            if app == 'image_viewer':
                q.type(f'run /bin/{app}.wiz', pause=0)
                tick_path = OUT / 'viewer-launch-tick.bin'

                def live_tick():
                    assert not q.monitor(f'memsave 0x{address("g_tick_count"):x} 8 "{tick_path}"')
                    return int.from_bytes(tick_path.read_bytes(), 'little')

                previous = live_tick()
                advanced_at = time.monotonic()
                deadline = advanced_at + 20
                longest_stall = 0
                q.monitor('sendkey ret 80')
                while scalar('g_window_count') == before:
                    tick = live_tick()
                    sampled_at = time.monotonic()
                    longest_stall = max(longest_stall, sampled_at - advanced_at)
                    if tick != previous:
                        previous = tick
                        advanced_at = sampled_at
                    assert sampled_at < deadline, 'Image Viewer did not launch'
                    time.sleep(.02)
                print(f'Image Viewer launch maximum sampled timer gap {longest_stall:.3f}s.', flush=True)
                assert longest_stall < .3, 'Image Viewer launch blocked timer interrupts'
            else:
                q.type(f'run /bin/{app}.wiz\n', pause=.5)
            wait_for(lambda: scalar('g_window_count') > before)
        overflow_slot = None
        for slot in range(8):
            click(start + slot * stride + 18, 800 - bar_height // 2)
            if scalar('g_taskbar_popup') == 1:
                overflow_slot = slot
                break
            q.monitor('sendkey esc 80')
            time.sleep(.2)
        assert overflow_slot is not None, 'Overflow button did not open its menu'
        bar = memory(buffer + (800 - bar_height) * 1280 * 4, 1280 * bar_height * 4)
        button_x = start + overflow_slot * stride
        dots = [(x, y) for y in range(6, bar_height - 6) for x in range(button_x + 2, button_x + 34)
                if struct.unpack_from('<I', bar, (y * 1280 + x) * 4)[0] == 0xffeeeeff]
        assert dots, 'Overflow dots are missing'
        assert min(x for x, y in dots) + max(x for x, y in dots) == 2 * button_x + 35, 'Dots are horizontally off-center'
        assert min(y for x, y in dots) + max(y for x, y in dots) == bar_height - 1, 'Dots are vertically off-center'
        q.command('screendump', {'filename': str(OUT / 'taskbar-overflow.ppm')})
        hidden = 8 - overflow_slot
        panel_y = 800 - bar_height - 40 - hidden * 32 - 4
        anchor = scalar('g_taskbar_anchor')
        panel_x = min(anchor, 1280 - 306)
        click(panel_x + 70, panel_y + 52)
        assert scalar('g_taskbar_popup') == 0, 'Overflow application was not selected'
        q.command('screendump', {'filename': str(OUT / 'terminal-complete.ppm')})
        print('Terminal QEMU checks passed including application grouping, window selection and overflow activation.', flush=True)
    finally:
        try:
            q.close()
        finally:
            if q.proc.poll() is None:
                q.proc.terminate()
                q.proc.wait(timeout=10)


if __name__ == '__main__':
    main()
