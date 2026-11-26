#!/usr/bin/env python3
"""Measure real app first paints; run after a normal GPU_RCS_ALPHA=1 ISO build.

An optional --boot-dir selects an isolated matching wyzar/wyzar.iso pair.
This measures app startup in QEMU, not physical GPU presentation performance.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import time
import qemu_storage_smoke as smoke
from qemu_audio_test import boot


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--boot-dir',type=Path,default=smoke.ROOT)
    args=parser.parse_args()
    smoke.ROOT=args.boot_dir.resolve()
    smoke.ISO_PATH=smoke.ROOT/'wyzar.iso';smoke.BOOT_ISO=True
    smoke.OUT.mkdir(exist_ok=True)
    symbols=subprocess.check_output(['nm','-n','-C',str(smoke.ROOT/'wyzar')],text=True).splitlines()
    def address(name):return next(int(line.split()[0],16) for line in symbols if line.split()[-1]==name)
    # Derive trace size/offsets from the actual declaration, avoiding a fixed
    # ABI assumption when launch instrumentation changes.
    helper=smoke.OUT/'startup-layout.cpp';binary=smoke.OUT/'startup-layout'
    helper.write_text('#include <cstdio>\n#include <cstddef>\n#include "src/drivers/gpu_launch_trace.h"\n'
        'int main(){printf("%zu %zu %zu %zu %zu %zu %zu",sizeof(GpuLaunchTrace),'
        'offsetof(GpuLaunchTrace,path),offsetof(GpuLaunchTrace,started),offsetof(GpuLaunchTrace,ready),'
        'offsetof(GpuLaunchTrace,created),offsetof(GpuLaunchTrace,painted),sizeof(g_gpu_launches)/sizeof(GpuLaunchTrace));}\n')
    subprocess.run(['clang++','-std=c++20','-I.',str(helper),'-o',str(binary)],check=True)
    size,path_at,start_at,ready_at,created_at,painted_at,capacity=map(int,subprocess.check_output([str(binary)]).split())
    q=smoke.Qemu(storage=False,resolution=(1280,800),extra_args=('-no-shutdown',))
    def memory(name,length):
        path=smoke.OUT/'startup-state.bin';q.command('stop')
        try:
            assert not q.monitor(f'memsave 0x{address(name):x} {length} "{path}"')
            return path.read_bytes()
        finally:q.command('cont')
    try:
        boot(q,'app-startup-boot')
        hz=int.from_bytes(memory('tsc_hz',8),'little');assert hz
        for app in ('notepad','task_manager','image_viewer','explorer'):
            path=f'/bin/{app}.wiz'
            q.type('run '+path+'\n',pause=.1)
            deadline=time.monotonic()+10
            while time.monotonic()<deadline:
                data=memory('g_gpu_launches',capacity*size)
                matches=[i*size for i in range(capacity)
                    if data[i*size+path_at:i*size+path_at+96].split(b'\0')[0].decode()==path]
                if matches:
                    offset=matches[-1]
                    values=[struct.unpack_from('<Q',data,offset+at)[0] for at in (start_at,ready_at,created_at,painted_at)]
                    if all(values):break
                time.sleep(.1)
            else:raise AssertionError(f'{app}: missing startup/first-paint milestone; use a normal GPU_RCS_ALPHA=1 build')
            assert values==sorted(values),(app,values)
            durations=[round((values[i+1]-values[i])*1000/hz,2) for i in range(3)]
            print(f'{app}: load={durations[0]} ms; ready-to-window={durations[1]} ms; window-to-paint={durations[2]} ms',flush=True)
        q.capture('app-startup-complete')
        assert q.command('query-status')['running']
        print('PASS: all four native apps reached first paint.',flush=True)
    finally:q.close()


if __name__=='__main__':main()
