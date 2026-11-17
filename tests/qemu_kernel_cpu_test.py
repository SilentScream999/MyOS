"""Exercise the real profiling syscall through Task Manager, including its lease."""
from pathlib import Path
import re
import struct
import subprocess
import time
import qemu_storage_smoke as smoke
from run_explorer_tests import save_png

ROOT=Path(__file__).resolve().parents[1]
smoke.BOOT_ISO=True
symbols=subprocess.check_output(['nm','-n','-C',str(ROOT/'wyzar')],text=True).splitlines()
def symbol(name):return next(int(line.split()[0],16) for line in symbols if line.split()[-1]==name)
source=(ROOT/'src/core/task.h').read_text()
task=('struct Task {'+source.split('struct Task {',1)[1].split('\n};',1)[0]+'\n};').replace('pid_t','uint32_t')
layout=smoke.OUT/'kernel-cpu-layout.cpp'
layout.write_text('#include <cstdio>\n#include <cstddef>\n#include <cstdint>\nstruct file;enum class TaskState:uint8_t;\n'
    '#define MAX_FDS '+re.search(r'#define MAX_FDS (\d+)',source)[1]+'\n'+task+
    '\nint main(){printf("%zu",offsetof(Task,cpu_cycles));}\n')
binary=smoke.OUT/'kernel-cpu-layout'
subprocess.run(['clang++','-std=c++20',str(layout),'-o',str(binary)],check=True)
cpu_offset=int(subprocess.check_output([str(binary)]))
q=smoke.Qemu(storage=False)
try:
    def memory(address,count):
        path=smoke.OUT/'kernel-cpu-state.bin'
        assert not q.monitor(f'memsave 0x{address:x} {count} "{path}"')
        return path.read_bytes()
    def value(name,count=8):return int.from_bytes(memory(symbol(name),count),'little')
    def snapshot():
        q.command('stop')
        try:
            # PID 0 remains the head of the circular task list.
            head=value('g_task_list');assert int.from_bytes(memory(head,4),'little')==0
            cpu=int.from_bytes(memory(head+cpu_offset,8),'little')
            info=struct.unpack('<16Q',memory(symbol('g_kernel_cpu'),128))
            return value('g_cpu_task_clock'),cpu,info
        finally:q.command('cont')
    for _ in range(120):
        time.sleep(.5)
        try:log=q.capture('kernel-cpu-boot')
        except AssertionError:continue
        if '[wm] Shell task started inside WM frame.' in log:break
    else:raise AssertionError('boot timeout')
    assert value('g_kernel_cpu_until_tick')==0
    q.type('run /bin/task_manager.wiz\n',pause=1)
    q.capture('kernel-cpu-launched')
    # Window creation places it on top; clicking its button also gives it input
    # focus, as a user would, before testing the keyboard return shortcut.
    for _ in range(40):
        dx=804-value('g_mouse_x',4);dy=158-value('g_mouse_y',4)
        if not dx and not dy:break
        q.monitor(f'mouse_move {max(-100,min(100,dx))} {max(-100,min(100,dy))}');time.sleep(.1)
    q.monitor('mouse_button 1');time.sleep(.1);q.monitor('mouse_button 0');time.sleep(2)
    q.capture('kernel-cpu-key')
    assert value('g_kernel_cpu_until_tick')>value('g_tick_count')
    first=snapshot();time.sleep(2);last=snapshot()
    elapsed=last[0]-first[0];cycles=[b-a for a,b in zip(first[2][2:9],last[2][2:9])]
    counted=sum(cycles);actual=last[1]-first[1]
    assert counted>0 and abs(counted-actual)<max(10000,actual//100),(counted,actual)
    calls=[b-a for a,b in zip(first[2][9:],last[2][9:])]
    assert calls[5]>0 and calls[6]>0 and calls[1]>0,calls
    labels=['Other','USB events','Hubs','Root ports','Key repeat','Interrupts','Scheduler']
    print('Kernel / Input profile: '+', '.join(f'{n} {c*100/elapsed:.2f}%' for n,c in zip(labels,cycles)),flush=True)
    q.capture('kernel-cpu-details');save_png(smoke.OUT/'kernel-cpu-details.ppm',smoke.OUT/'kernel-cpu-details.png')
    q.monitor('sendkey p');time.sleep(3)
    assert value('g_kernel_cpu_until_tick')<=value('g_tick_count')
    stopped=snapshot()[2];time.sleep(1);assert snapshot()[2]==stopped
    assert 'KERNEL PANIC' not in q.capture('kernel-cpu-passed')
    print('QEMU passed: real Task Manager profiling, exclusive categories sum to PID 0 CPU, interrupts/scheduler counted, lease stops profiling.',flush=True)
finally:q.close()
