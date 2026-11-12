"""A missing background hub completion must leave input, ticks and disk alive."""
from pathlib import Path
import subprocess
import sys
import time
import qemu_storage_smoke as smoke
from qemu_debug_memory import DebugMemory

ROOT = Path(__file__).resolve().parents[1]
smoke.BOOT_ISO = True
smoke.IMAGE = ROOT/'.storage-test/hub-async-timeout.img'
smoke.IMAGE.write_bytes((ROOT/'.storage-test/root.img').read_bytes())
symbols = subprocess.check_output(['nm', '-n', str(ROOT/'wyzar')], text=True)
def address(name):
    return next(int(line.split()[0], 16) for line in symbols.splitlines() if line.split()[-1].endswith(name))
source = ROOT/'.storage-test/hub-async-offsets.cpp'
source.write_text('''#include <cstddef>
#include <cstdio>
#include "src/drivers/usb_hub_watch.h"
int main() { std::printf("%zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu", sizeof(HubStatusWatch), offsetof(HubStatusWatch,scan_mask), offsetof(HubStatusWatch,phase),
offsetof(HubStatusWatch,poll)+offsetof(USBHubPollSchedule,audit_after),
offsetof(HubStatusWatch,control)+offsetof(USBHubControl,deadline),
offsetof(HubStatusWatch,control)+offsetof(USBHubControl,active),
offsetof(HubStatusWatch,control)+offsetof(USBHubControl,complete),
offsetof(HubStatusWatch,control)+offsetof(USBHubControl,setup_trb),
offsetof(HubStatusWatch,control)+offsetof(USBHubControl,data_trb),
offsetof(HubStatusWatch,control)+offsetof(USBHubControl,status_trb),
offsetof(HubStatusWatch,ready)); }
''')
subprocess.run(['clang++', '-std=c++20', '-I.', str(source), '-o', '.storage-test/hub-async-offsets'], cwd=ROOT, check=True)
size, scan, phase, audit, deadline, active, complete, setup, data, status, ready = map(int,
    subprocess.check_output([str(ROOT/'.storage-test/hub-async-offsets')], text=True).split())
q = smoke.Qemu(hub_depth=2, extra_args=('-gdb', 'tcp:127.0.0.1:45456'))
def scalar(name, length=8):
    path = ROOT/'.storage-test/hub-async-value.bin'
    assert not q.monitor(f'memsave 0x{address(name):x} {length} "{path}"')
    return int.from_bytes(path.read_bytes(), 'little')
try:
    for _ in range(100):
        time.sleep(.5)
        try: log = q.capture('hub-async-timeout-start')
        except AssertionError as error:
            if 'Invalid addr' in str(error): continue
            raise
        if '[wm] Shell task started inside WM frame.' in log: break
    else: raise AssertionError('boot timeout')
    assert '[storage] WYZARDATA mounted as root /.' in log
    mouse = next(m for m in q.command('query-mice') if 'HID' in m['name'] or 'USB' in m['name'])
    q.monitor(f"mouse_set {mouse['index']}")
    q.command('stop'); debug = DebugMemory()
    base = address('hub_watch') + size # Inner hub, containing the mounted disk.
    frequency = int.from_bytes(debug.read(address('tsc_hz'), 8), 'little')
    # Model a submitted request whose completion never arrives. No DMA buffers
    # are freed or hardware state altered; other devices keep their real rings.
    due = int.from_bytes(debug.read(base + audit, 8), 'little') + frequency
    debug.write(base + scan, (2).to_bytes(4, 'little'))
    debug.write(base + phase, b'\x01')
    debug.write(base + deadline, due.to_bytes(8, 'little'))
    for offset, value in ((setup, 0xbad000), (data, 0xbad010), (status, 0xbad020)):
        debug.write(base + offset, value.to_bytes(8, 'little'))
    debug.write(base + complete, b'\x00'); debug.write(base + active, b'\x01')
    debug.close(); q.command('cont')
    before_tick = scalar('g_tick_count'); before_mouse = scalar('g_mouse_x', 4)
    q.monitor('mouse_move 13 0'); time.sleep(.15)
    assert scalar('g_tick_count') > before_tick + 20, 'timer stalled during hub wait'
    assert scalar('g_mouse_x', 4) > before_mouse, 'mouse stalled during hub wait'
    q.type('write /async.txt disk-survived-background-timeout\n', pause=.5)
    log = q.capture('hub-async-timeout-end')
    assert log.count('[usb] Hub background status timeout; preserving other devices.') == 1
    assert 'KERNEL PANIC' not in log and '[storage] USB transport failed' not in log
    assert log.count('xHCI controller running.') == 1
    q.command('stop'); debug = DebugMemory()
    assert debug.read(base + ready, 1) == b'\x00'
    assert debug.read(address('hub_watch') + ready, 1) == b'\x01', 'healthy upstream monitor was disabled'
    debug.close(); q.command('cont')
finally:
    q.close()
sys.path.insert(0, str(ROOT/'tools'))
from system_volume import SystemVolume
from make_usb_image import START, BOOT_SECTORS, DATA_SECTORS
raw = smoke.IMAGE.read_bytes(); offset = (START + BOOT_SECTORS)*512
volume = SystemVolume(raw[offset:offset + DATA_SECTORS*512])
assert volume.read('async.txt') == b'disk-survived-background-timeout'
print('Missing async hub completion: timer and mouse progressed during wait; disk write persisted; healthy hub retained; no controller reset')
