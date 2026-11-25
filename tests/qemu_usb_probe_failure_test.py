"""Missing hub command/status completions must not permanently freeze the OS."""
from pathlib import Path
import subprocess
import os
import re
import struct
import sys
import time
import qemu_storage_smoke as smoke
from qemu_debug_memory import DebugMemory

ROOT = Path(__file__).resolve().parents[1]
smoke.BOOT_ISO = True
subprocess.run(['clang++','-std=c++20','-I.', 'tests/usb_probe_offsets.cpp',
                '-o','.storage-test/usb-probe-offsets'],cwd=ROOT,check=True)
command_phys_offset, ep0_offset, ring_phys_offset = map(int,subprocess.check_output(
    [str(ROOT/'.storage-test/usb-probe-offsets')],text=True).split())
symbols = subprocess.check_output(['nm','-n',str(ROOT/'wyzar')],text=True)
def address(needle):
    return next(int(line.split()[0],16) for line in symbols.splitlines() if needle in line.split()[-1])

def check(kind):
    smoke.IMAGE = ROOT/f'.storage-test/hub-{kind}-timeout.img'
    smoke.IMAGE.write_bytes((ROOT/'.storage-test/root.img').read_bytes())
    q = smoke.Qemu(extra_args=('-gdb','tcp:127.0.0.1:45456'))
    def scalar(name,size=4):
        path = ROOT/'.storage-test/probe-scalar.bin'
        assert not q.monitor(f'memsave 0x{address(name):x} {size} "{path}"')
        return int.from_bytes(path.read_bytes(),'little')
    try:
        for _ in range(100):
            time.sleep(.5)
            try: log=q.capture(f'hub-{kind}-start')
            except AssertionError as error:
                if 'Invalid addr' in str(error): continue
                raise
            if '[wm] Shell task started inside WM frame.' in log: break
        else: raise AssertionError('boot timeout')
        assert '[storage] WYZARDATA mounted as root /.' in log
        original_mouse=next(m for m in q.command('query-mice') if 'HID' in m['name'] or 'USB' in m['name'])
        q.command('stop')
        q.command('device_add',{'driver':'usb-hub','id':'faulty','bus':'xhci.0','port':'4'})
        if kind=='command':
            q.command('device_add',{'driver':'usb-mouse','id':'child','bus':'xhci.0','port':'4.1'})
        debug=DebugMemory()
        breakpoint=address('usb_wait_command' if kind=='command' else 'hub_get_port_status')
        assert debug.request(f'Z0,{breakpoint:x},1')=='OK'
        assert debug.request('c').startswith(('T05','S05'))
        if kind=='command':
            fault_address=address('xhci_hc')+command_phys_offset
        else:
            registers=q.monitor('info registers')
            match=re.search(r'\bRDI=([0-9a-fA-F]+)',registers)
            assert match, registers
            hub=int(match[1],16)
            ep0=int.from_bytes(debug.read(hub+ep0_offset,8),'little')
            fault_address=ep0+ring_phys_offset
        original=debug.read(fault_address,8)
        # Hardware still completes the actual queued TRB; the deliberately bad
        # software completion address models a completion the driver never sees.
        debug.write(fault_address,(int.from_bytes(original,'little')+16).to_bytes(8,'little'))
        tick=scalar('g_tick_count',8)
        composed=scalar('g_scene_compose_count',8)
        debug.close()
        time.sleep(.15)
        assert scalar('g_tick_count',8)>tick+20,'Timer stalled inside USB discovery'
        assert scalar('g_scene_compose_count',8)>composed,'Desktop stalled inside USB discovery'
        expected='Command completion timeout:' if kind=='command' else '[usb] Failed hub probe;'
        started=time.monotonic()
        for _ in range(100):
            time.sleep(.2)
            log=q.capture(f'hub-{kind}-failure')
            if expected in log: break
        else: raise AssertionError('probe froze instead of timing out: '+log[-3000:])
        assert time.monotonic()-started<6
        if kind=='control':assert 'FAIL: get_port_status (pre-reset) failed' in log
        q.command('stop')
        debug=DebugMemory(); debug.write(fault_address,original); debug.close()
        q.command('cont')
        # Timer progress no longer implies discovery has returned: its waits
        # are now preemptible. Wait for bounded cleanup before testing input.
        for _ in range(60):
            time.sleep(.1)
            if not scalar('usb_control_depth',4):break
        else:raise AssertionError('synchronous USB ownership leaked after cleanup')
        before=scalar('g_tick_count',8)
        for _ in range(25):
            time.sleep(.1)
            if scalar('g_tick_count',8)>before: break
        else: raise AssertionError('scheduler interrupts never recovered after bounded cleanup')
        assert not q.monitor(f"mouse_set {original_mouse['index']}")
        before=scalar('g_mouse_x'); assert not q.monitor('mouse_move 19 0'); time.sleep(.3)
        assert scalar('g_mouse_x')>before, 'existing USB mouse did not recover'
        q.type('write /probe.txt desktop-survived\n',pause=.5)
        log=q.capture(f'hub-{kind}-end')
        assert log.count('xHCI controller running.')==1
        assert 'KERNEL PANIC' not in log
    finally:q.close()
    sys.path.insert(0,str(ROOT/'tools'))
    from image_io import copy_sparse
    partition=ROOT/f'.storage-test/hub-{kind}-timeout-ext4.img'
    with smoke.IMAGE.open('rb') as source,partition.open('wb') as target:
        mbr=source.read(512);start,sectors=struct.unpack_from('<II',mbr,446+16+8)
        source.seek(start*512);copy_sparse(source,target,sectors*512)
    assert subprocess.check_output(['debugfs','-R','cat /probe.txt',str(partition)],stderr=subprocess.DEVNULL)==b'desktop-survived'
    print(f'Hub {kind} timeout: desktop and timer progressed during discovery; bounded failure preserved mouse, keyboard and boot disk')
    partition.unlink()
    smoke.IMAGE.unlink()

for kind in (os.environ['PROBE_KIND'],) if 'PROBE_KIND' in os.environ else ('command','control'):
    check(kind)
