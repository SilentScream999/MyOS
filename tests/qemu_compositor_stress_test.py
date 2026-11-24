"""Launch real apps over animated alpha/Rainbow fixtures; inspect heap/stacks.

Uses the mock GPU backend. It checks lifetime/preemption, not Intel DMA or speed.
"""
from pathlib import Path
import struct
import subprocess
import time
import qemu_storage_smoke as smoke
from qemu_debug_memory import DebugMemory

ROOT=Path(__file__).resolve().parents[1]

def main():
    symbols=subprocess.check_output(['nm','-n','-C','wyzar'],text=True).splitlines()
    def address(name):
        return next(int(line.split()[0],16) for line in symbols if line.split()[-1]==name)
    address('g_gpu_test_render_calls')
    task_source=(ROOT/'src/core/task.h').read_text()
    task=('struct Task {'+task_source.split('struct Task {',1)[1].split('\n};',1)[0]+'\n};').replace('pid_t','uint32_t')
    helper=smoke.OUT/'stress-layout.cpp';binary=smoke.OUT/'stress-layout'
    helper.write_text('#include <cstddef>\n#include <cstdint>\n#include <cstdio>\n#include "src/drivers/gpu_perf.h"\n'
        '#define MAX_FDS 16\nstruct file;enum class TaskState:uint8_t;\n'+task+
        'int main(){printf("%zu %zu %zu %zu %zu %zu",offsetof(Task,state),offsetof(Task,next),offsetof(Task,stack_base),offsetof(Task,stack_size),offsetof(Task,wake_at_tick),offsetof(GpuPerfCounters,cpu_alpha_draws));}\n')
    subprocess.run(['clang++','-std=c++20','-I.',str(helper),'-o',str(binary)],check=True)
    state,nxt,stack_base,stack_size,wake,cpu_alpha=map(int,subprocess.check_output([str(binary)]).split())
    smoke.BOOT_ISO=True
    q=smoke.Qemu(storage=False,resolution=(1280,800),extra_args=('-no-shutdown','-gdb','tcp:127.0.0.1:45456'))
    debug=None
    def stop():
        nonlocal debug
        q.command('stop');debug=DebugMemory()
    def resume():
        nonlocal debug
        debug.close();debug=None;q.command('cont')
    def value(name,size=8):return int.from_bytes(debug.read(address(name),size),'little')
    def windows():
        return [(w,debug.read(w+4,64).split(b'\0')[0]) for w in
                struct.unpack('<'+'Q'*value('g_window_count',4),debug.read(address('g_window_list'),value('g_window_count',4)*8))]
    def launch(path):
        stop()
        head=value('desktop_launch_head',4);tail=value('desktop_launch_tail',4)
        assert (head+1)%8!=tail
        debug.write(address('desktop_launch_paths')+head*256,path.encode()+b'\0')
        debug.write(address('desktop_launch_head'),((head+1)%8).to_bytes(4,'little'))
        worker=value('desktop_launcher_task')
        debug.write(worker+state,b'\0');debug.write(worker+wake,bytes(8))
        resume()
    try:
        for _ in range(160):
            time.sleep(.25)
            try:log=q.capture('compositor-stress-boot')
            except AssertionError:continue
            if '[wm] Shell task started inside WM frame.' in log:break
        else:raise AssertionError('Boot did not finish')
        launch('/bin/gpu_test.elf')
        for _ in range(80):
            time.sleep(.1);stop()
            gpu=next((w for w,title in windows() if title==b'GPU Scaling Test'),0)
            if gpu:
                debug.write(address('g_focused_win_id'),debug.read(gpu,4));resume();break
            resume()
        else:raise AssertionError('GPU test launch failed')
        q.type('a ',pause=.5)
        stop()
        pixels=int.from_bytes(debug.read(gpu+344,8),'little')
        assert debug.read(pixels+(40*320+40)*4,4)==bytes(4),'Alpha test did not activate'
        rain_before=value('g_rain_render_count')
        alpha_before=int.from_bytes(debug.read(address('g_gpu_perf')+cpu_alpha,8),'little')
        resume()
        for path in ['/bin/notepad.wiz','/bin/explorer.wiz','/bin/task_manager.wiz','/bin/gpu_test.elf']*2:
            launch(path);time.sleep(.7)
        time.sleep(5);stop()
        titles=[title for _,title in windows()]
        assert value('g_rain_render_count')>rain_before,'Rainbow stopped during app launches'
        assert int.from_bytes(debug.read(address('g_gpu_perf')+cpu_alpha,8),'little')>alpha_before,'SIMD alpha did not execute'
        assert titles.count(b'GPU Scaling Test')>=3,titles
        assert any(b'Notepad' in title for title in titles),titles
        assert any(b'Explorer' in title for title in titles),titles
        assert any(b'Task Manager' in title for title in titles),titles
        pointer=value('g_heap_head');previous=0;blocks=0
        while pointer:
            size,free,prior,following,magic,reserved=struct.unpack('<QB7xQQQQ',debug.read(pointer,48))
            assert magic==0xDEADBEEFCAFEBABE and prior==previous and size>=48 and size%16==0,hex(pointer)
            previous,pointer=pointer,following;blocks+=1
            assert blocks<100000
        assert previous==value('g_heap_tail')
        head=value('g_task_list');pointer=head;tasks=0
        for _ in range(128):
            base=int.from_bytes(debug.read(pointer+stack_base,8),'little')
            size=int.from_bytes(debug.read(pointer+stack_size,8),'little')
            if base and size:assert int.from_bytes(debug.read(base,8),'little')==0x57595a415253544b
            pointer=int.from_bytes(debug.read(pointer+nxt,8),'little');tasks+=1
            if pointer==head:break
        else:raise AssertionError('Task list cycle')
        resume();log=q.capture('compositor-stress-passed')
        assert 'CORRUPTION' not in log and 'stack overflow' not in log and 'KERNEL PANIC' not in log
        print(f'Overlapping app launch stress passed: {len(titles)} windows, {tasks} tasks, {blocks} intact heap headers.',flush=True)
    finally:
        if debug:debug.close()
        q.close()

if __name__=='__main__':main()
