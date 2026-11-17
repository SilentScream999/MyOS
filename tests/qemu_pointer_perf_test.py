"""Measure real pointer/ghost/resize work without animated windows. --baseline records only."""
from pathlib import Path
import argparse
import json
import re
import struct
import subprocess
import time
import qemu_storage_smoke as smoke

ROOT=Path(__file__).resolve().parents[1]
baseline=argparse.ArgumentParser();baseline.add_argument('--baseline',action='store_true');args=baseline.parse_args()
symbols=subprocess.check_output(['nm','-n','-C',str(ROOT/'wyzar')],text=True).splitlines()
def address(name):return next(int(l.split()[0],16) for l in symbols if l.split()[-1]==name)
wm=(ROOT/'src/gui/wm.h').read_text();ts=(ROOT/'src/core/task.h').read_text()
event='struct WinEvent {'+wm.split('struct WinEvent {',1)[1].split('\n};',1)[0]+'\n};'
window=('struct Window {'+wm.split('struct Window {',1)[1].split('\n};',1)[0]+'\n};').replace('pid_t','uint32_t')
task=('struct Task {'+ts.split('struct Task {',1)[1].split('\n};',1)[0]+'\n};').replace('pid_t','uint32_t')
layout=smoke.OUT/'pointer-perf-layout.cpp';exe=smoke.OUT/'pointer-perf-layout'
layout.write_text('#include <cstdio>\n#include <cstddef>\n#include <cstdint>\n#include "src/gui/window_resize.h"\n'
    '#define WIN_EVENT_QUEUE_SIZE 64\n#define MAX_FDS '+re.search(r'#define MAX_FDS (\d+)',ts)[1]+
    '\nstruct file;enum class TaskState:uint8_t;\n'+event+window+task+
    '\nint main(){printf("%zu %zu %zu %zu %zu %zu %zu %zu %zu",sizeof(Window),offsetof(Window,x),offsetof(Window,y),offsetof(Window,w),offsetof(Window,h),offsetof(Window,ghost_dragging),offsetof(Window,resize_edges),offsetof(Task,next),offsetof(Task,cpu_cycles));}\n')
subprocess.run(['clang++','-std=c++20','-I.',str(layout),'-o',str(exe)],check=True)
size,xo,yo,wo,ho,ghost,resize,nxt,cpu=map(int,subprocess.check_output([str(exe)]).split())
smoke.BOOT_ISO=True;q=smoke.Qemu(storage=False,resolution=(1280,800))
results={}
try:
    def memory(p,n):
        path=smoke.OUT/'pointer-perf-state.bin';assert not q.monitor(f'memsave 0x{p:x} {n} "{path}"');return path.read_bytes()
    def value(name,n=8):return int.from_bytes(memory(address(name),n),'little')
    def rect(name):
        d=memory(value(name),size);return tuple(struct.unpack_from('<i',d,o)[0] for o in (xo,yo,wo,ho))
    def move(x,y):
        for _ in range(60):
            dx=x-value('g_mouse_x',4);dy=y-value('g_mouse_y',4)
            if not dx and not dy:return
            q.monitor(f'mouse_move {max(-80,min(80,dx))} {max(-80,min(80,dy))}');time.sleep(.04)
        raise AssertionError('pointer target not reached')
    def button(mask):q.monitor(f'mouse_button {mask}');time.sleep(.15)
    def click(x,y):move(x,y);button(1);button(0);time.sleep(.2)
    def sample():
        q.command('stop')
        try:
            head=value('g_task_list');p=head
            while int.from_bytes(memory(p,4),'little')!=1:
                p=int.from_bytes(memory(p+nxt,8),'little');assert p!=head
            return (value('g_scene_compose_count'),value('g_scene_compose_pixels'),
                    int.from_bytes(memory(p+cpu,8),'little'),value('g_cpu_task_clock'),
                    value('term_cursor_transitions') if not args.baseline else 0)
        finally:q.command('cont')
    def measure(label,moving):
        start=sample()
        for i in range(90):
            if moving:q.monitor('mouse_move '+('3 2' if i%2==0 else '-3 -2'))
            time.sleep(.035)
        end=sample();seconds=(end[3]-start[3])/value('tsc_hz')
        result={'seconds':seconds,'scene_calls':end[0]-start[0],'scene_pixels':end[1]-start[1],
                'compositor_cpu':(end[2]-start[2])*100/(end[3]-start[3]),'caret_transitions':end[4]-start[4]}
        results[label]=result;print(label,result,flush=True)
        if not args.baseline:assert result['scene_calls']<=seconds*5+4,(label,result)
        if not args.baseline and label in ('idle','pointer'):assert result['caret_transitions']==0,result
    for _ in range(120):
        time.sleep(.5)
        try:log=q.capture('pointer-perf-boot')
        except AssertionError:continue
        if '[wm] Shell task started inside WM frame.' in log:break
    else:raise AssertionError('boot timeout')
    title=value('g_title_h',4);height=800
    # Close the three builtin fixtures using real taskbar/title controls.
    for index,name in ((3,'g_win_log'),(2,'g_win_rain'),(1,'g_win_test')):
        start=6+value('g_sb_w',4)+10*height//1080;bw=value('g_wbtn_w',4);gap=4*height//1080
        click(start+index*(bw+gap)+bw//2,height-15)
        x,y,w,h=rect(name);click(x+w-max(2,title//10)-(title+8)//2,y-title//2)
    click(40,120);time.sleep(.5) # Inactive terminal: no caret damage.
    measure('idle',False);measure('pointer',True)
    x,y,w,h=rect('g_win_terminal');move(x+100,y-title//2);button(1);time.sleep(.3)
    assert memory(value('g_win_terminal')+ghost,1)!=b'\0'
    measure('ghost_drag',True);assert rect('g_win_terminal')==(x,y,w,h);button(0);time.sleep(.3)
    move(x+w-2,y+h-2);button(1);time.sleep(.3)
    assert int.from_bytes(memory(value('g_win_terminal')+resize,4),'little')
    measure('ghost_resize',True);assert rect('g_win_terminal')==(x,y,w,h);button(0)
    if not args.baseline:
        click(x+20,y+25);q.type('run /bin/task_manager.wiz\n',pause=1)
        pointer=int.from_bytes(memory(address('g_window_list')+(value('g_window_count',4)-1)*8,8),'little')
        data=memory(pointer,size);app=tuple(struct.unpack_from('<i',data,o)[0] for o in (xo,yo,wo,ho))
        cx=value('g_term_ox',4)+value('term_x',4)*8;cy=value('g_term_oy',4)+value('term_y',4)*10+8
        assert app[0]<=cx<app[0]+app[2] and app[1]<=cy<app[1]+app[3],(app,cx,cy)
        assert value('g_focused_win_id',4)==int.from_bytes(memory(value('g_win_terminal'),4),'little')
        assert value('term_cursor_enabled',1)==0,'Covered shell caret still blinks'
        before=value('term_cursor_transitions');time.sleep(1.2);assert value('term_cursor_transitions')==before
        click(app[0]+app[2]-max(2,title//10)-(title+8)//2,app[1]-title//2)
        time.sleep(.4);assert value('term_cursor_enabled',1)==1,'Uncovered caret did not restart'
    assert 'KERNEL PANIC' not in q.capture('pointer-perf-passed')
    (smoke.OUT/('pointer-perf-baseline.json' if args.baseline else 'pointer-perf.json')).write_text(json.dumps(results,indent=2))
finally:q.close()
