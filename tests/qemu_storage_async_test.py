#!/usr/bin/env python3
"""During a real USB disk timeout, timer, mouse and keyboard must keep running."""
from pathlib import Path
import shutil
import subprocess
import time
import threading
import qemu_storage_smoke as smoke
from qemu_debug_memory import DebugMemory

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.storage-test'


def main():
    smoke.BOOT_ISO = True
    smoke.IMAGE = OUT / 'async-storage.img'
    shutil.copyfile(ROOT / 'wyzar-usb.img', smoke.IMAGE)
    symbols = subprocess.check_output(['nm', '-C', str(ROOT / 'wyzar')], text=True).splitlines()

    def address(name):
        return next(int(line.split()[0], 16) for line in symbols if line.split()[-1] == name)

    wait_address = next(int(line.split()[0], 16) for line in symbols if 'usb_storage_wait(' in line)
    fixture = OUT / 'storage-completion-offsets.cpp'
    fixture.write_text('''#include <cstddef>
#include <cstdio>
#include "src/drivers/usb_storage_completion.h"
int main() { printf("%zu %zu %zu", sizeof(USBStorageCompletion),
offsetof(USBStorageCompletion,active), offsetof(USBStorageCompletion,pointer)); }
''')
    binary = OUT / 'storage-completion-offsets'
    subprocess.run(['clang++', '-std=c++20', '-I.', str(fixture), '-o', str(binary)], cwd=ROOT, check=True)
    size, active_offset, pointer_offset = map(int, subprocess.check_output([str(binary)], text=True).split())
    q = smoke.Qemu(hub_depth=2, extra_args=('-gdb', 'tcp:127.0.0.1:45456'))

    def memory(addr, length):
        path = OUT / 'async-storage-memory.bin'
        assert not q.monitor(f'memsave 0x{addr:x} {length} "{path}"')
        return path.read_bytes()

    def scalar(name, length=8):
        return int.from_bytes(memory(address(name), length), 'little')

    try:
        for _ in range(80):
            time.sleep(.5)
            try:
                log = q.capture('async-storage-start')
            except AssertionError as error:
                if 'Invalid addr' in str(error):
                    continue
                raise
            if '[init] Decoupled terminal boot.' in log:
                break
        else:
            raise AssertionError(log[-4000:])
        assert '[storage] ext4 journal active on USB system root.' in log
        q.type('write /home/async.txt waiting', pause=.1)
        q.command('stop')
        debug = DebugMemory()
        assert debug.request(f'Z0,{wait_address:x},1') == 'OK'
        # QEMU drops synthetic keys sent while paused. Resume through GDB,
        # then submit Enter while the guest is actually running.
        enter = threading.Timer(.2, lambda: q.monitor('sendkey ret 40'))
        enter.start()
        stop = debug.request('c')
        enter.join()
        assert stop.startswith(('T05', 'S05')), stop
        base = address('usb_storage_completions')
        raw = memory(base, size * 256 * 4)
        active = [i for i in range(256 * 4) if raw[i * size + active_offset]]
        assert len(active) == 1, active
        entry = base + active[0] * size
        pointer = int.from_bytes(raw[active[0] * size + pointer_offset:active[0] * size + pointer_offset + 8], 'little')
        # Keep the real hardware/rings intact; deny only this waiter's expected
        # completion. It must time out while other devices stay operational.
        debug.write(entry + pointer_offset, (pointer ^ (1 << 40)).to_bytes(8, 'little'))
        assert debug.request(f'z0,{wait_address:x},1') == 'OK'
        debug.close()
        q.command('cont')
        tick = scalar('g_tick_count')
        mouse = next(m for m in q.command('query-mice') if 'HID' in m['name'] or 'USB' in m['name'])
        q.monitor(f"mouse_set {mouse['index']}")
        before_screen = OUT / 'async-storage-before.ppm'
        after_screen = OUT / 'async-storage-after.ppm'
        q.command('screendump', {'filename': str(before_screen)})
        before_x = scalar('g_mouse_x', 4)
        before_keys = scalar('tty_out_head', 4)
        before_line = scalar('tty_line_len', 4)
        q.monitor('mouse_move 13 0')
        q.monitor('sendkey a 500')
        time.sleep(.15)
        assert memory(entry + active_offset, 1) == b'\x01', 'Wait ended before responsiveness measurement'
        assert scalar('g_tick_count') > tick + 20, 'Timer stalled during storage wait'
        assert scalar('g_mouse_x', 4) > before_x, 'Mouse stalled during storage wait'
        assert (scalar('tty_out_head', 4) != before_keys or scalar('tty_line_len', 4) != before_line), 'Keyboard stalled during storage wait'
        q.command('screendump', {'filename': str(after_screen)})
        assert before_screen.read_bytes() != after_screen.read_bytes(), 'Desktop presentation stalled during storage wait'
        for _ in range(50):
            time.sleep(.1)
            log = q.capture('async-storage-end')
            if '[storage] USB transport failed; disk offline until reboot.' in log:
                break
        else:
            raise AssertionError(log[-4000:])
        assert 'KERNEL PANIC' not in log
        assert memory(entry + active_offset, 1) == b'\0'
        print('Actual USB timeout: timer, keyboard, mouse and desktop presentation progressed; bounded failure offlined only the disk')
    except Exception:
        q.capture('async-storage-failure')
        raise
    finally:
        q.close()
        smoke.IMAGE.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
