#!/usr/bin/env python3
"""Regression: map the firmware framebuffer before the first splash write.

Uses QEMU's default VGA mode, matching make run, and -no-shutdown so a triple
fault is reported as a stopped VM instead of being mistaken for a slow boot.
Run after make iso. System-image writes use temporary copies only.
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import qemu_storage_smoke as smoke

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from image_io import copy_sparse


def main():
    smoke.OUT.mkdir(exist_ok=True)
    symbols=subprocess.check_output(['nm','-n','-C',str(ROOT/'wyzar')],text=True).splitlines()
    retired=next(int(line.split()[0],16) for line in symbols if line.split()[-1]=='g_recovery_retired')
    with tempfile.TemporaryDirectory(prefix='wyzar-framebuffer-') as directory:
        image=Path(directory)/'usb.img'
        for label,iso,storage,resolution in (
            ('boot-iso-default-vga',True,True,None),
            ('boot-usb-fixed-vga',False,True,(1024,768)),
            ('boot-recovery-default-vga',True,False,None),
        ):
            if storage:
                with (ROOT/'wyzar-usb.img').open('rb') as source,image.open('wb') as target:
                    copy_sparse(source,target,(ROOT/'wyzar-usb.img').stat().st_size)
            smoke.IMAGE=image; smoke.BOOT_ISO=iso
            q=smoke.Qemu(storage=storage,resolution=resolution,extra_args=('-no-shutdown',))
            ready_at=None; log=''
            try:
                for _ in range(160):
                    time.sleep(.5)
                    status=q.command('query-status')
                    assert status.get('running'),(label,status,q.monitor('info registers'))
                    try: log=q.capture(label)
                    except AssertionError: continue # Kernel symbols are not mapped until handoff.
                    if '[wm] Shell task started inside WM frame.' in log:
                        if ready_at is None: ready_at=time.monotonic()
                        if time.monotonic()-ready_at>=3: break
                else: raise AssertionError(label+': '+log[-4000:])
                mapped='[boot] Framebuffer mapped before first draw.'
                assert mapped in log and log.index('IDT loaded')<log.index(mapped)
                state=smoke.OUT/f'{label}-retired.bin'
                assert not q.monitor(f'memsave 0x{retired:x} 1 "{state}"')
                assert state.read_bytes()==(b'\1' if storage else b'\0')
                q.type('write /tmp/fbtest alive\n',pause=.2)
                q.type('cat /tmp/fbtest\n',pause=.3)
                assert q.command('query-status')['running']
                q.capture(label+'-working')
                print(label+': boot, shell I/O and recovery lifetime passed.',flush=True)
            finally:
                q.close()
    smoke.BOOT_ISO=False


if __name__=='__main__': main()
