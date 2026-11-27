#!/usr/bin/env python3
"""Exercise the bundled power command on unsupported and emulated Intel CPUs.

Reports come from the real guest filesystem; no disks are attached or modified.
QEMU does not establish battery behaviour or actual hardware clock frequencies.
"""
import time
import struct
import subprocess
import qemu_storage_smoke as smoke
from qemu_audio_test import boot

def main():
    smoke.OUT.mkdir(exist_ok=True);smoke.BOOT_ISO=True
    symbols=subprocess.check_output(['nm','-n','-C',str(smoke.ROOT/'wyzar')],text=True).splitlines()
    root_address=next(int(line.split()[0],16) for line in symbols if line.split()[-1]=='ramfs_root_node')
    power_status=next(int(line.split()[0],16) for line in symbols if line.split()[-1]=='g_cpu_power_boot')
    # Derive the prefix offsets from the actual RAMFS declaration. Read the
    # saved file directly, since terminal output is not part of the kernel log.
    source=(smoke.ROOT/'src/fs/ramfs.h').read_text()
    prefix='struct ramfs_node {'+source.split('struct ramfs_node {',1)[1].split('// Abstract interface',1)[0]+'};'
    prefix=prefix.replace('VnodeType','int')
    helper=smoke.OUT/'power-layout.cpp';binary=smoke.OUT/'power-layout'
    helper.write_text('#include <cstdint>\n#include <cstddef>\n#include <cstdio>\n'+prefix+
        '\nint main(){printf("%zu %zu %zu %zu %zu",offsetof(ramfs_node,children),offsetof(ramfs_node,sibling),'
        'offsetof(ramfs_node,data),offsetof(ramfs_node,size),sizeof(ramfs_node));}\n')
    subprocess.run(['clang++',str(helper),'-o',str(binary)],check=True)
    children,sibling,data,size,node_bytes=map(int,subprocess.check_output([str(binary)]).split())
    # Retain the emulator's supported feature set; only change CPUID identity
    # to exercise the Intel client-model gate without unsupported AVX/VMX flags.
    for label,cpu in (('generic','qemu64'),('intel','qemu64,vendor=GenuineIntel,family=6,model=142,stepping=9')):
        q=smoke.Qemu(storage=False,extra_args=('-no-shutdown','-cpu',cpu))
        def memory(address,count):
            output=smoke.OUT/'power-memory.bin'
            assert not q.monitor(f'memsave 0x{address:x} {count} "{output}"')
            return output.read_bytes()
        def saved(name):
            q.command('stop')
            try:
                pointer=int.from_bytes(memory(root_address,8),'little')
                for component in ('home',name):
                    pointer=struct.unpack_from('<Q',memory(pointer,node_bytes),children)[0]
                    while pointer:
                        node=memory(pointer,node_bytes)
                        if node[:256].split(b'\0')[0].decode()==component:break
                        pointer=struct.unpack_from('<Q',node,sibling)[0]
                    if not pointer:return None
                pointer=struct.unpack_from('<Q',node,data)[0];count=struct.unpack_from('<Q',node,size)[0]
                return memory(pointer,count).decode() if count else ''
            finally:q.command('cont')
        def wait_report(name):
            deadline=time.monotonic()+25
            while time.monotonic()<deadline:
                result=saved(name)
                if result and '[cpu_work_including_scheduling]' in result:return result
                time.sleep(.25)
            q.capture('power-'+label+'-failed')
            raise AssertionError('Report not saved: '+name)
        try:
            boot(q,'power-'+label+'-boot')
            q.command('stop')
            try:assert int.from_bytes(memory(power_status,4),'little')==(0 if label=='generic' else 1),'Boot policy must skip unsupported/virtual CPUs'
            finally:q.command('cont')
            q.type('power /home/power-'+label+'.txt\n',pause=5)
            log=wait_report('power-'+label+'.txt')
            (smoke.OUT/('power-'+label+'-report.txt')).write_text(log)
            for expected in ('MyOS power report v1','work_iterations=32000000',
                             '[observation]','[cpu_work_including_scheduling]',
                             'gpu_frequency_status=unavailable'):
                assert expected in log,(expected,log[-8000:])
            if label=='generic':assert 'HWP_REQUEST=unavailable' in log,log[-8000:]
            else:assert 'vendor=GenuineIntel' in log and 'PLATFORM_INFO=0x' in log,log[-8000:]
            # Repeat after the first process exited: no owner leak or background
            # capture is allowed to block another terminal/process.
            q.type('power /home/power-repeat.txt\n',pause=5)
            assert 'MyOS power report v1' in wait_report('power-repeat.txt')
            assert q.command('query-status')['running']
            print('PASS: '+label+' report saved, read back and repeated; guest remains responsive',flush=True)
        finally:
            try:q.close()
            except subprocess.TimeoutExpired:
                q.proc.kill();q.proc.wait();raise

if __name__=='__main__':main()
