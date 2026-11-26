#!/usr/bin/env python3
"""Capture/export and cancellation in a diskless production guest; no E470 claims."""
import subprocess
import sys
import time
import qemu_storage_smoke as smoke
from qemu_audio_test import boot
from audio_service_probe import AudioServiceProbe
sys.path.insert(0,str(smoke.ROOT/'tools'))
from read_fn_capture import decode

def main():
    smoke.OUT.mkdir(exist_ok=True);smoke.BOOT_ISO=True
    symbols=subprocess.check_output(['nm','-n','-C','wyzar'],text=True).splitlines()
    def address(name):return next(int(line.split()[0],16) for line in symbols if line.split()[-1]==name)
    source=(smoke.ROOT/'src/fs/ramfs.h').read_text();vfs=(smoke.ROOT/'src/fs/vfs.h').read_text()
    node='struct ramfs_node {'+source.split('struct ramfs_node {',1)[1].split('\n};',1)[0]+'\n};'
    vnode=('struct vnode {'+vfs.split('struct vnode {',1)[1].split('\n};',1)[0]+'\n};').replace('vfs_ops*','void*')
    helper=smoke.OUT/'fn-layout.cpp';binary=smoke.OUT/'fn-layout'
    helper.write_text('#include <cstddef>\n#include <cstdint>\n#include <cstdio>\n#include "src/drivers/fn_trace.h"\nenum class VnodeType{FILE,DIRECTORY,DEVICE};\n'+vnode+node+
        'int main(){printf("%zu %zu %zu %zu %zu %zu",offsetof(ramfs_node,children),offsetof(ramfs_node,sibling),offsetof(ramfs_node,data),offsetof(ramfs_node,size),offsetof(ramfs_node,opens),offsetof(FnTrace,deadline));}\n')
    subprocess.run(['clang++','-std=c++20','-I.',str(helper),'-o',str(binary)],check=True)
    children,sibling,data,size,opens,deadline=map(int,subprocess.check_output([str(binary)]).split())
    q=smoke.Qemu(storage=False,input_devices=False,extra_args=('-no-shutdown',))
    def memory(pointer,length):
        path=smoke.OUT/'fn-memory.bin';q.command('stop')
        try:
            assert not q.monitor(f'memsave 0x{pointer:x} {length} "{path}"')
            return path.read_bytes()
        finally:q.command('cont')
    def number(pointer,length=8):return int.from_bytes(memory(pointer,length),'little')
    def child(parent,name):
        pointer=number(parent+children)
        for _ in range(512):
            if not pointer:return 0
            if memory(pointer,256).split(b'\0')[0]==name.encode():return pointer
            pointer=number(pointer+sibling)
        raise AssertionError('Directory cycle')
    try:
        boot(q,'fn-capture-boot')
        service=AudioServiceProbe(q);service.assert_idle()
        q.type('audio hotkeys\n',pause=1)
        for key in ('f1','f2','f3','volumeup'):
            q.command('send-key',{'keys':[{'type':'qcode','data':key}],'hold-time':60});time.sleep(.15)
        root=number(address('ramfs_root_node'));home=0
        for _ in range(30):
            home=child(root,'home')
            if home:break
            time.sleep(.1)
        assert home
        file=0
        for _ in range(100):
            time.sleep(.5);file=child(home,'fnkeys.bin')
            if file and number(file+size)>16 and number(file+opens,4)==0:break
        else:raise AssertionError('Capture file was not saved/closed')
        result=memory(number(file+data),number(file+size))
        (smoke.OUT/'fn-qemu-capture.bin').write_bytes(result)
        trace,dropped,flags,tables=decode(result)
        assert not dropped and flags==0
        assert {0,2}.issubset(tables),tables.keys()
        assert {e[3] for e in trace if e[2]==4}==set(range(10))
        raw=[e[1] for e in trace if e[2]==0]
        assert all(key in raw for key in (0x3b,0x3c,0x3d,0xe0)),raw
        assert not any(e[2] in (1,2,3,6,7) for e in trace),'Firmware I/O ran on a non-E470 guest'
        q.type('audio hotkeys\n',pause=1)
        assert number(address('g_fn_trace')+deadline)>0
        q.monitor('sendkey ctrl-c 80');time.sleep(1)
        assert number(address('g_fn_trace')+deadline)==0,'Cancelled capture kept recording'
        assert q.command('query-status')['running']
        service.assert_idle()
        print(f'PASS: bounded raw PS/2 capture, 10 stages, {len(tables)} checksummed ACPI tables, saved bundle, unsupported-platform firmware exclusion and cancellation')
    finally:q.close()

if __name__=='__main__':main()
