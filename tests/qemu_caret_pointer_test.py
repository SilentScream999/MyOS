"""Caret damage over an underlying title bar must not corrupt pointer restoration."""
from pathlib import Path
import re
import struct
import subprocess
import time
import qemu_storage_smoke as smoke
from run_explorer_tests import save_png

ROOT=Path(__file__).resolve().parents[1]
symbols=subprocess.check_output(['nm','-n','-C',str(ROOT/'wyzar')],text=True).splitlines()
def address(name):return next(int(l.split()[0],16) for l in symbols if l.split()[-1]==name)
wm=(ROOT/'src/gui/wm.h').read_text();ts=(ROOT/'src/core/task.h').read_text()
event='struct WinEvent {'+wm.split('struct WinEvent {',1)[1].split('\n};',1)[0]+'\n};'
window=('struct Window {'+wm.split('struct Window {',1)[1].split('\n};',1)[0]+'\n};').replace('pid_t','uint32_t')
task=('struct Task {'+ts.split('struct Task {',1)[1].split('\n};',1)[0]+'\n};').replace('pid_t','uint32_t')
code=smoke.OUT/'caret-pointer-layout.cpp';exe=smoke.OUT/'caret-pointer-layout'
code.write_text('#include <cstdio>\n#include <cstddef>\n#include <cstdint>\n#include "limine.h"\n#include "src/gui/window_resize.h"\n'
    '#define WIN_EVENT_QUEUE_SIZE 64\n#define MAX_FDS '+re.search(r'#define MAX_FDS (\d+)',ts)[1]+
    '\nstruct file;enum class TaskState:uint8_t;\n'+event+window+task+
    '\nint main(){printf("%zu %zu %zu %zu %zu %zu",offsetof(Window,y),offsetof(limine_framebuffer,pitch),offsetof(Task,next),offsetof(Task,state),offsetof(Window,x),offsetof(Window,w));}\n')
subprocess.run(['clang++','-std=c++20','-I.',str(code),'-o',str(exe)],check=True)
wy,pitch_offset,nxt,state,wx,ww=map(int,subprocess.check_output([str(exe)]).split())
smoke.BOOT_ISO=True;q=smoke.Qemu(storage=False,resolution=(1280,800))
try:
    def memory(p,n):
        path=smoke.OUT/'caret-pointer-state.bin';assert not q.monitor(f'memsave 0x{p:x} {n} "{path}"');return path.read_bytes()
    def value(name,n=8):return int.from_bytes(memory(address(name),n),'little')
    def move(x,y):
        for _ in range(50):
            dx=x-value('g_mouse_x',4);dy=y-value('g_mouse_y',4)
            if not dx and not dy:return
            q.monitor(f'mouse_move {max(-80,min(80,dx))} {max(-80,min(80,dy))}');time.sleep(.04)
        raise AssertionError('pointer target not reached')
    for _ in range(120):
        time.sleep(.5)
        try:log=q.capture('caret-pointer-boot')
        except AssertionError:continue
        if '[wm] Shell task started inside WM frame.' in log:break
    else:raise AssertionError('boot timeout')
    ox=value('g_term_ox',4);oy=value('g_term_oy',4);width=value('g_term_max_cols',4)*8
    title=value('g_title_h',4)
    behind=value('g_win_log');under_y=int.from_bytes(memory(behind+wy,4),'little')
    under_x=int.from_bytes(memory(behind+wx,4),'little');under_w=int.from_bytes(memory(behind+ww,4),'little')
    # Put the caret's bottom damage row through an underlying window's title.
    for _ in range(70):
        cy=oy+value('term_y',4)*10+8
        if under_y-title+2<=cy+2<under_y-2:break
        assert cy+2<under_y,'Prompt has already passed the target title bar'
        q.type('\n',pause=.1)
    else:raise AssertionError('Could not align caret with the underlying title bar')
    left=max(ox+24,under_x+8);right=min(ox+width-24,under_x+under_w-8)
    assert left+64<right
    pitch=int.from_bytes(memory(value('fb')+pitch_offset,8),'little')
    head=value('g_task_list');compositor=head
    while int.from_bytes(memory(compositor,4),'little')!=1:
        compositor=int.from_bytes(memory(compositor+nxt,8),'little');assert compositor!=head
    start=value('term_cursor_transitions');checked=0
    for i in range(50):
        move(left+32+(i%8)*12,cy-5);time.sleep(.06)
        q.command('stop')
        try:
            if memory(compositor+state,1)!=b'\x02':continue
            # Compare the saved scene after a complete frame, not midway through
            # composing the background windows. The pointer never belongs here.
            for y in range(cy,cy+4):
                scene=memory(value('g_master_backbuffer')+y*pitch+left*4,(right-left)*4)
                expected=memory(value('g_backbuffer')+((y-oy)*width+left-ox)*4,(right-left)*4)
                assert scene==expected,(i,y,cy,scene[:32].hex(),expected[:32].hex())
            checked+=1
        finally:q.command('cont')
    assert checked>=10 and value('term_cursor_transitions')-start>=2,(checked,start,value('term_cursor_transitions'))
    q.capture('caret-pointer-passed');save_png(smoke.OUT/'caret-pointer-passed.ppm',smoke.OUT/'caret-pointer-passed.png')
    print(f'Caret/pointer regression passed: {checked} complete-frame row checks across multiple blinks; no background-window pixels.',flush=True)
except Exception:
    q.capture('caret-pointer-failed');save_png(smoke.OUT/'caret-pointer-failed.ppm',smoke.OUT/'caret-pointer-failed.png')
    raise
finally:q.close()
