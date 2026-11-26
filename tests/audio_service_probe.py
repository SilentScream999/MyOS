"""Observe the actual guest worker/task accounting, without waking it."""
import re
import subprocess
import time
import qemu_storage_smoke as smoke


class AudioServiceProbe:
    def __init__(self,q):
        self.q=q
        symbols=subprocess.check_output(['nm','-n','-C','wyzar'],text=True).splitlines()
        self.symbols={line.split()[-1]:int(line.split()[0],16) for line in symbols if len(line.split())>=3}
        # Compile offsets from the actual PCB declaration rather than pinning
        # byte offsets or requiring a debugger installation.
        source=(smoke.ROOT/'src/core/task.h').read_text()
        start=source.index('enum class TaskState');enum=source[start:source.index('};',start)+2]
        task='struct Task {'+source.split('struct Task {',1)[1].split('\n};',1)[0]+'\n};'
        task=task.replace('pid_t','uint32_t').replace('file*','void*')
        maxfds=re.search(r'#define MAX_FDS (\d+)',source)[1]
        helper=smoke.OUT/'audio-service-layout.cpp';binary=smoke.OUT/'audio-service-layout'
        helper.write_text('#include <cstdint>\n#include <cstddef>\n#include <cstdio>\n#define MAX_FDS '+maxfds+'\n'+enum+'\n'+task+
            '\nint main(){printf("%zu %zu %zu %zu",offsetof(Task,cpu_cycles),offsetof(Task,state),offsetof(Task,wake_at_tick),sizeof(Task::state));}\n')
        subprocess.run(['clang++','-std=c++20',str(helper),'-o',str(binary)],check=True)
        self.cpu,self.state,self.deadline,self.state_size=map(int,subprocess.check_output([str(binary)]).split())
    def read(self,address,size):
        path=smoke.OUT/'audio-service-probe.bin'
        self.q.command('stop')
        try:assert not self.q.monitor(f'memsave 0x{address:x} {size} "{path}"')
        finally:self.q.command('cont')
        return int.from_bytes(path.read_bytes(),'little')
    def number(self,name,size=8):return self.read(self.symbols[name],size)
    def snapshot(self):
        task=self.number('hda::service_thread')
        return (self.number('hda::service_iterations'),self.read(task+self.cpu,8),
                self.read(task+self.deadline,8),self.read(task+self.state,self.state_size))
    def assert_idle(self,seconds=1):
        for _ in range(50):
            before=self.snapshot()
            if before[2:]==(0,2):break # BLOCKED, event wake only
            time.sleep(.1)
        else:raise AssertionError(('Audio worker did not block indefinitely',before))
        tick=self.number('g_tick_count');time.sleep(seconds)
        after=self.snapshot()
        assert self.number('g_tick_count')>tick,'Guest clock stopped during idle measurement'
        assert before==after,('Idle audio worker consumed CPU or polled',before,after)
        return after
