"""Ordinary USB mouse: both axes and continuous window drag while buttons vary."""
from pathlib import Path
import subprocess
import time
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]
smoke.IMAGE = ROOT / '.storage-test/mouse-drag.img'
smoke.IMAGE.write_bytes((ROOT / '.storage-test/root.img').read_bytes())
symbols = subprocess.check_output(['nm','-n',str(ROOT/'wyzar')], text=True)
def address(name):
    return next(int(line.split()[0],16) for line in symbols.splitlines() if line.split()[-1].endswith(name))
q = smoke.Qemu()
def scalar(name, size=4):
    path = ROOT / '.storage-test/drag-scalar.bin'
    assert not q.monitor(f'memsave 0x{address(name):x} {size} "{path}"')
    return int.from_bytes(path.read_bytes(),'little',signed=size==4)
def move_to(x,y):
    for _ in range(50):
        dx, dy = x-scalar('g_mouse_x'), y-scalar('g_mouse_y')
        if abs(dx)<=1 and abs(dy)<=1: return
        assert not q.monitor(f'mouse_move {max(-70,min(70,dx))} {max(-70,min(70,dy))}')
        time.sleep(.04)
    raise AssertionError('mouse did not reach target')
try:
    for _ in range(100):
        time.sleep(.5)
        try: log = q.capture('mouse-drag-start')
        except AssertionError as error:
            if 'Invalid addr' in str(error): continue
            raise
        if '[wm] Shell task started inside WM frame.' in log: break
    else: raise AssertionError('boot timeout')
    assert '[storage] WYZARDATA mounted as root /.' in log
    mouse = next(m for m in q.command('query-mice') if 'HID' in m['name'] or 'USB' in m['name'])
    assert not q.monitor(f"mouse_set {mouse['index']}")
    old_x, old_y = scalar('g_term_ox'), scalar('g_term_oy')
    move_to(old_x+100, old_y-scalar('g_title_h')//2)
    start_x, start_y = scalar('g_mouse_x'), scalar('g_mouse_y')
    assert not q.monitor('mouse_button 1')
    time.sleep(.2)
    for mask in (1,3,5,7,1,5,3,1):
        assert not q.monitor(f'mouse_button {mask}')
        for _ in range(5):
            assert not q.monitor('mouse_move 4 3')
            time.sleep(.035)
            assert scalar('g_mouse_buttons',1) & 1
            assert scalar('g_term_ox') == old_x and scalar('g_term_oy') == old_y, 'drag committed before release'
    dx,dy = scalar('g_mouse_x')-start_x, scalar('g_mouse_y')-start_y
    assert dx>0 and dy>0
    assert not q.monitor('mouse_button 0')
    time.sleep(.4)
    assert scalar('g_term_ox') == old_x+dx
    assert scalar('g_term_oy') == old_y+dy
    q.type('write /drag.txt held-button-stable\n', pause=.5)
    log = q.capture('mouse-drag-end')
    assert 'KERNEL PANIC' not in log
finally: q.close()
print('QEMU mouse: both axes, held-left drag with other buttons, and single final drop passed')
