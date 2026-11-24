#!/usr/bin/env python3
"""Real TTY sleep/wakeup, private desktop launch and CPU accounting in QEMU."""
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import time
import qemu_storage_smoke as smoke

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from image_io import copy_sparse


def main():
    symbols=subprocess.check_output(['nm','-n','-C',str(ROOT/'wyzar')],text=True).splitlines()
    def symbol(name):
        return next(int(line.split()[0],16) for line in symbols if line.split()[-1]==name)
    usage=next(int(line.split()[0],16) for line in symbols if line.endswith('::usage'))
    with tempfile.TemporaryDirectory(prefix='wyzar-cpu-idle-') as directory:
        work=Path(directory); image=work/'usb.img'; partition=work/'partition.img'
        with (ROOT/'wyzar-usb.img').open('rb') as source,image.open('wb') as target:
            copy_sparse(source,target,(ROOT/'wyzar-usb.img').stat().st_size)
        with image.open('rb') as source:
            mbr=source.read(512); start,sectors=struct.unpack_from('<II',mbr,446+16+8)
            source.seek(start*512)
            with partition.open('wb') as target: copy_sparse(source,target,sectors*512)
        prefix='''typedef unsigned long u64;
static long call(u64 n,u64 a,u64 b,u64 c) { long r;
 __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r; }
'''
        for name,body in (
            ('cpubusy','void _start(void) { for(;;) __asm__ volatile("pause"); }'),
            ('cpureader','void _start(void) { char c=0; if(call(0,0,(u64)&c,1)!=1 || c!=\'a\') for(;;){} call(60,0,0,0); for(;;){} }'),
        ):
            source=work/f'{name}.c'; source.write_text(prefix+body)
            obj=work/f'{name}.o'; exe=work/f'{name}.elf'
            subprocess.run(['clang','-O1','-ffreestanding','-fno-stack-protector','-fno-pic','-fno-pie','-mno-red-zone','-target','x86_64-elf','-c',str(source),'-o',str(obj)],check=True)
            subprocess.run(['ld.lld','-nostdlib','-Ttext=0x400000','-o',str(exe),str(obj)],check=True)
            manifest=work/'manifest'; manifest.write_text(f'put /bin/{name}.elf {exe}\n')
            subprocess.run([str(smoke.OUT/'ext4-image-tool'),str(partition),str(manifest)],check=True)
        with partition.open('rb') as source,image.open('r+b') as target:
            target.seek(start*512); copy_sparse(source,target,sectors*512)

        # Derive the PCB layout from its actual declaration, not assumed offsets.
        task_source=(ROOT/'src/core/task.h').read_text()
        task='struct Task {'+task_source.split('struct Task {',1)[1].split('\n};',1)[0]+'\n};'
        task=task.replace('pid_t','uint32_t')
        max_fds=re.search(r'#define MAX_FDS (\d+)',task_source)[1]
        console_source=(ROOT/'src/core/app_console.h').read_text()
        console=('struct AppConsole {'+console_source.split('struct AppConsole {',1)[1].split('\n};',1)[0]+'\n};').replace('pid_t','uint32_t')
        layout=work/'layout.cpp'
        layout.write_text('#include <cstdio>\n#include <cstddef>\n#include <cstdint>\n'
            '#include "src/gui/resources.h"\n#include "limine.h"\n'
            f'#define MAX_FDS {max_fds}\nstruct file; enum class TaskState:uint8_t;\n'+task+console+
            '\nint main(){printf("%zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu",sizeof(Task),offsetof(Task,state),offsetof(Task,next),offsetof(Task,waiting_tty_input),offsetof(Task,wait_pid),offsetof(resources::Catalog,apps),sizeof(resources::Entry),offsetof(resources::App,exec),offsetof(limine_framebuffer,height),offsetof(Task,wake_at_tick),offsetof(Task,executable),offsetof(Task,console),offsetof(AppConsole,foreground));}\n')
        binary=work/'layout'
        subprocess.run(['clang++','-std=c++20','-I',str(ROOT),str(layout),'-o',str(binary)],check=True)
        size,state,next_offset,waiting,wait_pid,apps,entry_size,exec_offset,height_offset,wake,executable,console_offset,foreground_offset=map(int,subprocess.check_output([str(binary)],text=True).split())
        smoke.IMAGE=image; smoke.BOOT_ISO=True
        # Leave enough visible Start rows for the larger bundled app catalog.
        q=smoke.Qemu(resolution=(1280,1080),extra_args=('-no-shutdown',))
        try:
            def memory(address,count):
                out=smoke.OUT/'cpu-idle-state.bin'
                assert not q.monitor(f'memsave 0x{address:x} {count} "{out}"')
                return out.read_bytes()
            def value(name,count=8): return int.from_bytes(memory(symbol(name),count),'little')
            def task_data(pid):
                head=value('g_task_list'); pointer=head
                for _ in range(128):
                    data=memory(pointer,size)
                    if struct.unpack_from('<I',data)[0]==pid: return data
                    pointer=struct.unpack_from('<Q',data,next_offset)[0]
                    if pointer==head: break
                raise AssertionError(f'No task {pid}')
            def wait_for(condition,message,seconds=8):
                deadline=time.monotonic()+seconds
                while time.monotonic()<deadline:
                    assert q.command('query-status')['running']
                    if condition(): return
                    time.sleep(.05)
                raise AssertionError(message)
            for _ in range(150):
                time.sleep(.5)
                try: log=q.capture('cpu-idle-boot')
                except AssertionError: continue
                if '[wm] Shell task started inside WM frame.' in log: break
            else: raise AssertionError(log[-4000:])
            shell=value('g_terminal_shell_pid',4)
            def foreground():
                data=task_data(shell)
                channel=struct.unpack_from('<Q',data,console_offset)[0]
                pid=int.from_bytes(memory(channel+foreground_offset,4),'little') if channel else value('g_terminal_foreground_pid',4)
                return 0 if pid==shell else pid
            def at_prompt():
                data=task_data(shell)
                return data[state]==2 and data[waiting]==1 and not foreground()
            def cpu(): return struct.unpack_from('<I',memory(usage,24),16)[0]
            def samples(seconds=2):
                result=[]
                for _ in range(int(seconds*10)):
                    assert 0<=cpu()<=100
                    result.append(cpu()); time.sleep(.1)
                return result
            wait_for(at_prompt,'Shell is spinning instead of blocking for input')
            time.sleep(1); baseline=samples()
            assert at_prompt()

            q.type('run /bin/counter.elf\n',pause=0)
            wait_for(lambda:foreground()!=0,'Counter did not start')
            counter=foreground()
            parent=task_data(shell)
            assert parent[state]==2 and parent[waiting]==0 and struct.unpack_from('<I',parent,wait_pid)[0]==counter
            during=samples(1.5)
            wait_for(at_prompt,'Counter exit did not restore a sleeping prompt')
            after=samples()
            print(f'HUD CPU: prompt {baseline}; Counter {during}; returned prompt {after}',flush=True)

            q.type('run /bin/cpureader.elf\n',pause=0)
            wait_for(lambda:foreground()!=0,'Reader did not start')
            reader=foreground()
            wait_for(lambda:task_data(reader)[waiting]==1,'Child stdin read did not block')
            q.monitor('sendkey a'); time.sleep(.2)
            assert task_data(reader)[state]==2 # Partial canonical line is not ready.
            q.monitor('sendkey ret')
            wait_for(at_prompt,'Completed line did not wake the child and restore prompt')
            q.type('run /bin/cpureader.elf\n',pause=0)
            wait_for(lambda:foreground()!=0,'Second reader did not start')
            reader=foreground()
            wait_for(lambda:task_data(reader)[waiting]==1,'Second read did not block')
            q.monitor('sendkey ctrl-c'); wait_for(at_prompt,'Ctrl+C did not cancel blocked stdin')

            q.type('run /bin/cpubusy.elf\n',pause=0)
            wait_for(lambda:foreground()!=0,'Busy process did not start')
            sample_start=struct.unpack_from('<Q',memory(usage,24))[0]
            frequency=value('tsc_hz')
            wait_for(lambda:struct.unpack_from('<Q',memory(usage,24))[0]-sample_start>=2*frequency,
                     'HUD did not sample two complete busy intervals',seconds=12)
            first=(value('g_tick_count'),value('g_idle_tsc_accum'),struct.unpack('<QQI4x',memory(usage,24)))
            busy=samples(1.5)
            last=(value('g_tick_count'),value('g_idle_tsc_accum'),struct.unpack('<QQI4x',memory(usage,24)))
            diagnostics=[]; head=value('g_task_list'); task_pointer=head
            for _ in range(128):
                data=memory(task_pointer,size)
                diagnostics.append((struct.unpack_from('<I',data)[0],data[state],struct.unpack_from('<Q',data,wake)[0]))
                task_pointer=struct.unpack_from('<Q',data,next_offset)[0]
                if task_pointer==head: break
            assert min(busy)>=95,('Busy execution was credited as idle',busy,first,last,
                diagnostics)
            q.monitor('sendkey ctrl-c'); wait_for(at_prompt,'Busy cancellation did not restore prompt')

            # Start launches a private desktop console; the shared shell must
            # remain asleep instead of acquiring its foreground process.
            fb=value('fb'); height=int.from_bytes(memory(fb+height_offset,8),'little')
            def move(x,y):
                for _ in range(60):
                    mx=value('g_mouse_x',4); my=value('g_mouse_y',4)
                    if (mx,my)==(x,y): return
                    q.monitor(f'mouse_move {max(-100,min(100,x-mx))} {max(-100,min(100,y-my))}'); time.sleep(.1)
                raise AssertionError('Pointer target not reached')
            def click():
                q.monitor('mouse_button 1'); time.sleep(.2); q.monitor('mouse_button 0'); time.sleep(.2)
            move(20,height-20); click()
            wait_for(lambda:value('g_menu_open',1)==1,'Start did not open')
            time.sleep(.4)
            catalog=value('g_resources_catalog'); count=int.from_bytes(memory(catalog,4),'little')
            entries=memory(catalog+apps,count*entry_size)
            index=next(i for i in range(count) if entries[i*entry_size+exec_offset:].startswith(b'/bin/counter.elf\0'))
            row=max(26,height*38//1080); header=max(44,height*56//1080); pad=max(10,height*14//1080)
            panel_height=min(header+max(1,count)*row+pad*2+max(52,height*64//1080),height*740//1080)
            panel_y=max(0,height-value('g_taskbar_h',4)-panel_height-height*4//1080)
            move(20,panel_y+header+pad+index*row+row//2); click()
            def desktop_counter_running():
                head=value('g_task_list');pointer=head
                for _ in range(128):
                    data=memory(pointer,size)
                    if data[executable:executable+256].split(b'\0')[0]==b'/bin/counter.elf' and data[state]!=3:return True
                    pointer=struct.unpack_from('<Q',data,next_offset)[0]
                    if pointer==head:break
                return False
            wait_for(desktop_counter_running,'Menu request did not launch Counter in a private desktop console')
            assert at_prompt(),'Desktop launch disturbed the sleeping shared shell'
            wait_for(lambda:not desktop_counter_running(),'Desktop Counter did not exit')
            assert at_prompt()
            q.capture('cpu-idle-passed')
            print(f'HUD CPU samples: prompt {baseline}; Counter {during}; returned prompt {after}; busy loop {busy}',flush=True)
            print('QEMU passed: blocked prompt, Counter, canonical input, read cancellation, busy accounting and private Start launch.',flush=True)
        finally: q.close()


if __name__=='__main__': main()
