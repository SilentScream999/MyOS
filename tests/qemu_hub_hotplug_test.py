"""Hotplug below one/two hubs while retaining a boot disk on a sibling port."""
from pathlib import Path
import subprocess
import sys
import time
import qemu_storage_smoke as smoke
from qemu_debug_memory import DebugMemory

ROOT=Path(__file__).resolve().parents[1]
smoke.BOOT_ISO=True
symbols=subprocess.check_output(['nm','-n',str(ROOT/'wyzar')],text=True)
layout_source=ROOT/'.storage-test/hub-watch-layout.cpp'
layout_source.write_text('#include <cstddef>\n#include <cstdio>\n#include "src/core/structures.h"\n'
    '#include "src/drivers/usb_hub_watch.h"\n'
    'int main(){std::printf("%zu %zu",sizeof(HubStatusWatch),offsetof(HubStatusWatch,pipe_failed));}\n')
subprocess.run(['clang++','-std=c++20','-I.',str(layout_source),'-o','.storage-test/hub-watch-layout'],cwd=ROOT,check=True)
watch_size,pipe_failed_offset=map(int,subprocess.check_output([str(ROOT/'.storage-test/hub-watch-layout')],text=True).split())
def address(name):
    return next(int(line.split()[0],16) for line in symbols.splitlines() if line.split()[-1].endswith(name))

def check(depth):
    smoke.IMAGE=ROOT/f'.storage-test/hub-local-{depth}.img'
    smoke.IMAGE.write_bytes((ROOT/'.storage-test/root.img').read_bytes())
    parent='1'+'.1'*(depth-1)
    q=smoke.Qemu(hub_depth=depth,input_devices=False,extra_args=(
        '-device','usb-mouse,id=steady,bus=xhci.0,port=3','-gdb','tcp:127.0.0.1:45456'))
    def scalar(name):
        path=ROOT/'.storage-test/hub-local-scalar.bin'
        assert not q.monitor(f'memsave 0x{address(name):x} 4 "{path}"')
        return int.from_bytes(path.read_bytes(),'little')
    def wait_change(count):
        for _ in range(100):
            time.sleep(.2)
            log=q.capture(f'hub-local-{depth}-change')
            if log.count('[usb] Hub port change handled locally; slot:')>=count:
                time.sleep(.3); return
        raise AssertionError('no local hub hotplug: '+log[-3500:])
    def add(driver='usb-kbd'):
        q.command('device_add',{'driver':driver,'id':'plug','bus':'xhci.0','port':parent+'.2'})
    try:
        for _ in range(100):
            time.sleep(.5)
            try:log=q.capture(f'hub-local-{depth}-start')
            except AssertionError as error:
                if 'Invalid addr' in str(error):continue
                raise
            if '[wm] Shell task started inside WM frame.' in log:break
        else:raise AssertionError('boot timeout')
        assert '[storage] WYZARDATA mounted as root /.' in log
        assert '[hid] Relative pointer ready (descriptor-based reports).' in log
        root_mouse=next(m for m in q.command('query-mice') if 'HID' in m['name'] or 'USB' in m['name'])
        q.command('device_add',{'driver':'usb-mouse','id':'sibling','bus':'xhci.0','port':parent+'.3'})
        wait_change(1)
        original=next(m for m in q.command('query-mice') if ('HID' in m['name'] or 'USB' in m['name']) and m['index']!=root_mouse['index'])
        assert not q.monitor(f"mouse_set {original['index']}")
        if depth==2:
            # After the current status transfer completes, suppress rearming.
            # Later replacements must be detected by fallback status polling.
            q.command('stop');debug=DebugMemory()
            debug.write(address('hub_watch')+(depth-1)*watch_size+pipe_failed_offset,b'\x01')
            debug.close();q.command('cont')
        changes=1
        for cycle in range(3):
            add();changes+=1;wait_change(changes)
            q.type(f'write /child{cycle}.txt hub-sibling-disk-alive\n',pause=.5)
            before=scalar('g_mouse_x');q.monitor('mouse_move 9 0');time.sleep(.2)
            assert scalar('g_mouse_x')>before, 'sibling mouse stalled'
            q.command('device_del',{'id':'plug'});changes+=1;wait_change(changes)
        # Coalesced change: keyboard -> mouse -> keyboard with no empty sample.
        add();changes+=1;wait_change(changes)
        q.command('stop');q.command('device_del',{'id':'plug'});add('usb-mouse');q.command('cont')
        changes+=1;wait_change(changes)
        new_mouse=next(m for m in q.command('query-mice') if ('HID' in m['name'] or 'USB' in m['name']) and m['index'] not in (original['index'],root_mouse['index']))
        q.monitor(f"mouse_set {new_mouse['index']}")
        before=scalar('g_mouse_y');q.monitor('mouse_move 0 11');time.sleep(.3)
        assert scalar('g_mouse_y')>before, 'replacement mouse did not work'
        q.command('stop');q.command('device_del',{'id':'plug'});add();q.command('cont')
        changes+=1;wait_change(changes)
        q.type('write /swapped.txt instant-hub-swap-working\n',pause=.5)
        log=q.capture(f'hub-local-{depth}-end')
        assert log.count('xHCI controller running.')==1
        assert log.count('[usb] Hub port change handled locally; slot:')==changes
        assert 'KERNEL PANIC' not in log
    finally:q.close()
    sys.path.insert(0,str(ROOT/'tools'))
    from system_volume import SystemVolume
    from make_usb_image import START,BOOT_SECTORS,DATA_SECTORS
    raw=smoke.IMAGE.read_bytes();offset=(START+BOOT_SECTORS)*512
    volume=SystemVolume(raw[offset:offset+DATA_SECTORS*512])
    for cycle in range(3):assert volume.read(f'child{cycle}.txt')==b'hub-sibling-disk-alive'
    assert volume.read('swapped.txt')==b'instant-hub-swap-working'
    print(f'Hub depth {depth}: accessory hotplug and rapid replacement retained sibling boot disk and mouse')

check(1)
check(2)
