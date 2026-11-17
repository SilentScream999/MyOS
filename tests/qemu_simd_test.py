#!/usr/bin/env python3
"""Verify real guest SSE state survives syscalls and timer preemption.

Build: make iso PERSIST_DISK=.storage-test/viewer-seed.img ROOT_MIB=64
Run: python3 tests/qemu_simd_test.py
"""
from pathlib import Path
import struct
import subprocess
import sys
import time
import qemu_storage_smoke as smoke

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'.storage-test'
sys.path.insert(0,str(ROOT/'tools'))
from image_io import copy_sparse

def main():
    obj=OUT/'simd-probe.o';elf=OUT/'simd-probe.elf'
    subprocess.run(['clang++','-std=c++20','-O2','-ffreestanding','-fno-builtin','-fno-stack-protector',
        '-fno-pic','-fno-pie','-mno-red-zone','-target','x86_64-elf','-c','tests/simd_probe.cpp','-o',str(obj)],check=True)
    subprocess.run(['ld.lld','-nostdlib','-z','max-page-size=0x1000','-Ttext=0x400000','-o',str(elf),str(obj)],check=True)
    seed=OUT/'viewer-seed.img';image=OUT/'simd-qemu.img';partition=OUT/'simd-partition.img'
    with seed.open('rb') as source,image.open('wb') as target:copy_sparse(source,target,seed.stat().st_size)
    with image.open('rb') as source,partition.open('wb') as target:
        mbr=source.read(512);start,sectors=struct.unpack_from('<II',mbr,470)
        source.seek(start*512);copy_sparse(source,target,sectors*512)
    manifest=OUT/'simd-files.txt';manifest.write_text(f'put /bin/simd_probe.elf {elf}\n')
    subprocess.run([str(OUT/'ext4-image-tool'),str(partition),str(manifest)],check=True)
    with image.open('r+b') as target,partition.open('rb') as source:
        target.seek(start*512);copy_sparse(source,target,sectors*512)
    symbols=subprocess.check_output(['nm','-n','-C',str(ROOT/'wyzar')],text=True).splitlines()
    matrix=next(int(l.split()[0],16) for l in symbols if l.split()[-1]=='g_term_matrix')
    smoke.IMAGE=image;smoke.BOOT_ISO=True;q=smoke.Qemu(resolution=(1280,800))
    try:
        for _ in range(100):
            time.sleep(.5)
            try:log=q.capture('simd-boot')
            except AssertionError:continue
            if '[wm] Shell task started inside WM frame.' in log:break
        else:raise AssertionError('boot timeout')
        q.type('run /bin/simd_probe.elf\n',pause=.5)
        deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            path=OUT/'simd-terminal.bin';assert not q.monitor(f'memsave 0x{matrix:x} 640000 "{path}"')
            terminal=path.read_bytes()[::8].replace(b'\0',b' ').decode(errors='replace')
            if 'SIMD PASS' in terminal:print('SIMD PASS: XMM0-15, MXCSR and x87 preserved across syscalls and timer preemption.',flush=True);return
            if 'SIMD FAIL' in terminal:raise AssertionError(terminal.strip())
            time.sleep(.2)
        raise AssertionError('SIMD probe timed out')
    finally:q.close()

if __name__=='__main__':main()
