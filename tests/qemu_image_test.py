#!/usr/bin/env python3
"""Boot disposable PNG/JPEG desktops and replace a PNG/manifest in the live guest.

Usage: python3 tests/qemu_image_test.py .storage-test/image-runtime.img
Requires run_image_tests.py fixtures and a current kernel in the supplied image.
"""
from pathlib import Path
import json
import struct
import subprocess
import sys
import tempfile
import time
import qemu_storage_smoke as smoke

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from make_usb_image import START, BOOT_SECTORS
from image_io import copy_sparse


def main():
    base=Path(sys.argv[1]).resolve()
    smoke.IMAGE=smoke.OUT/'image-test.img'
    offset=(START+BOOT_SECTORS)*512
    symbols=subprocess.check_output(['nm','-n','-C',str(ROOT/'wyzar')],text=True).splitlines()

    def address(name):
        return next(int(line.split()[0],16) for line in symbols if len(line.split())==3 and line.split()[2]==name)

    with tempfile.TemporaryDirectory(prefix='wyzar-image-test-') as temporary:
        work=Path(temporary)
        partition=work/'partition.img'
        with base.open('rb') as source, partition.open('wb') as target:
            source.seek(offset); copy_sparse(source,target,base.stat().st_size-offset)

        def put(path,content):
            asset=work/'asset'; asset.write_bytes(content)
            manifest=work/'manifest'; manifest.write_text(f'put {path} {asset}\n')
            subprocess.run([str(smoke.OUT/'ext4-image-tool'),str(partition),str(manifest)],check=True)

        manifest=work/'manifest'; manifest.write_text('mkdir /apps/pictures\n')
        subprocess.run([str(smoke.OUT/'ext4-image-tool'),str(partition),str(manifest)],check=True)
        initial=b'name=Gallery\nexec=/bin/win_test.elf\nicon=/home/icon.png\n'
        updated=b'name=Updated Gallery\nexec=/bin/win_test.elf\nicon=/home/icon.png\n'
        blue=(smoke.OUT/'image-fixtures/blue.png').read_bytes()
        code='''typedef unsigned long u64;
static long call(u64 n,u64 a,u64 b,u64 c) {
 long result; __asm__ volatile("syscall":"=a"(result):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return result;
}
'''
        code+='static const unsigned char png[]={'+','.join(map(str,blue))+'};\n'
        code+='static const char manifest[]='+json.dumps(updated.decode())+';\n'
        code+='''void _start(void) {
 int ok=call(138,(u64)"/home/icon.png",(u64)png,sizeof(png))==0;
 ok=ok && call(138,(u64)"/apps/pictures/app.manifest",(u64)manifest,sizeof(manifest)-1)==0;
 call(138,(u64)"/home/image-test.txt",(u64)(ok?"PASS":"FAIL"),4);
 call(60,!ok,0,0); for(;;) {}
}'''
        source=work/'update.c'; source.write_text(code)
        obj=work/'update.o'; exe=work/'update.elf'
        subprocess.run(['clang','-O1','-ffreestanding','-fno-stack-protector','-fno-pic','-fno-pie','-mno-red-zone','-target','x86_64-elf','-c',str(source),'-o',str(obj)],check=True)
        subprocess.run(['ld.lld','-nostdlib','-z','max-page-size=0x1000','-Ttext=0x400000','-o',str(exe),str(obj)],check=True)
        put('/bin/update.elf',exe.read_bytes())
        put('/apps/pictures/app.manifest',initial)
        put('/home/icon.png',(smoke.OUT/'image-fixtures/alpha.png').read_bytes())
        put('/home/wall.png',(smoke.OUT/'image-fixtures/adam7.png').read_bytes())
        put('/home/wall.jpg',(ROOT/'tests/fixtures/images/testorig.jpg').read_bytes())

        # Derive layout using the same public structs, rather than hand-coded offsets.
        layout_source=work/'layout.cpp'
        layout_source.write_text('''#include <cstdio>
#include <cstddef>
#include "src/gui/resources.h"
int main(){ printf("%zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu",sizeof(resources::Catalog),offsetof(resources::Catalog,apps),sizeof(resources::Entry),offsetof(resources::Entry,icons),offsetof(resources::App,name),offsetof(Image,pixels),offsetof(resources::Store,decodes),offsetof(resources::Store,hits),offsetof(resources::App,exec),offsetof(resources::Icons,title),offsetof(resources::Icons,task)); }
''')
        layout_exe=work/'layout'
        subprocess.run(['clang++','-std=c++20','-I',str(ROOT),str(layout_source),'-o',str(layout_exe)],check=True)
        total,apps,entry_size,icons,name,pixels,decodes,hits,exec_offset,title,task=map(int,subprocess.check_output([str(layout_exe)],text=True).split())

        def boot(label,wallpaper_ok=True):
            with base.open('rb') as source, smoke.IMAGE.open('wb') as target:
                copy_sparse(source,target,offset)
                with partition.open('rb') as volume: copy_sparse(volume,target,partition.stat().st_size)
            q=smoke.Qemu(resolution=(1024,768))

            def memory(addr,size):
                target=smoke.OUT/'image-state.bin'
                result=q.monitor(f'memsave 0x{addr:x} {size} "{target}"'); assert not result,result
                return target.read_bytes()

            def catalog():
                pointer=struct.unpack('<Q',memory(address('g_resources_catalog'),8))[0]
                return memory(pointer,total)

            def move(x,y):
                # Avoid saturating the virtual HID's report queue with a large
                # motion immediately after boot; wait for actual guest position.
                for _ in range(60):
                    mx=struct.unpack('<i',memory(address('g_mouse_x'),4))[0]
                    my=struct.unpack('<i',memory(address('g_mouse_y'),4))[0]
                    if mx==x and my==y: return
                    dx=max(-100,min(100,x-mx)); dy=max(-100,min(100,y-my))
                    q.monitor(f'mouse_move {dx} {dy}'); time.sleep(.1)
                raise AssertionError('Guest pointer did not reach target')

            def start():
                move(20,748)
                q.monitor('mouse_button 1'); time.sleep(.2); q.monitor('mouse_button 0'); time.sleep(.2)

            def gallery_index(data,display=b'Gallery\0'):
                count=struct.unpack_from('<I',data)[0]
                return next(i for i in range(count) if data[apps+i*entry_size+name:].startswith(display))

            def pointer(data,index,variant=0):
                return struct.unpack_from('<Q',data,apps+index*entry_size+icons+variant+pixels)[0]

            try:
                for _ in range(90):
                    time.sleep(.5)
                    try: log=q.capture(label)
                    except AssertionError: continue
                    if '[init] Decoupled terminal boot.' in log: break
                else: raise AssertionError(log[-4000:])
                expected='[wallpaper] Loaded disk asset into RAM.' if wallpaper_ok else '[wallpaper] Missing, unreadable or invalid asset; keeping fallback.'
                assert expected in log,log[-4000:]
                data=catalog(); count=struct.unpack_from('<I',data)[0]
                gallery=gallery_index(data)
                for i in range(count):
                    assert not pointer(data,i) and not pointer(data,i,title) and not pointer(data,i,task)
                for _ in range(100):
                    if memory(address('g_recovery_retired'),1)==b'\1': break
                    time.sleep(.05)
                else: raise AssertionError('Successful disk boot did not retire recovery')
                heap_bytes=struct.unpack('<Q',memory(address('g_recovery_heap_bytes'),8))[0]
                archive_bytes=struct.unpack('<Q',memory(address('g_recovery_archive_bytes'),8))[0]
                fallback_bytes=struct.unpack('<Q',memory(address('g_recovery_fallback_bytes'),8))[0]
                assert heap_bytes>0 and archive_bytes>0 and fallback_bytes>0,(heap_bytes,archive_bytes,fallback_bytes)
                assert memory(address('ramfs_root_node'),8)==bytes(8)
                print(f'{label}: reclaimed {heap_bytes} heap, {archive_bytes} archive and {fallback_bytes} fallback module bytes.',flush=True)
                # First actual demand for app PNGs: opening Start.
                start()
                for _ in range(100):
                    time.sleep(.05); data=catalog(); gallery=gallery_index(data)
                    if pointer(data,gallery): break
                else:
                    q.capture(label+'-demand-failed')
                    state={key:int.from_bytes(memory(address(key),size),'little') for key,size in
                           (('g_menu_open',1),('g_resources_requested',1),('g_resources_refresh',1),
                            ('g_resource_readers',4),('g_resource_demand_generation',8),('g_mouse_x',4),('g_mouse_y',4))}
                    raise AssertionError('Opening Start did not load the displayed app icon: '+str(state))
                icon_pointer=pointer(data,gallery)
                assert struct.unpack('<I',memory(icon_pointer,4))[0]==0x80800000
                assert not pointer(data,gallery,title) and not pointer(data,gallery,task)
                assert b'/bin/win_test.elf\0' in data
                before=struct.unpack('<Q',memory(address('g_resource_store')+decodes,8))[0]
                start() # Close the menu; app menu surfaces should be released.
                for _ in range(100):
                    time.sleep(.05); data=catalog()
                    if not pointer(data,gallery): break
                else: raise AssertionError('Closing Start retained app menu surfaces')
                assert struct.unpack('<Q',memory(address('g_resource_store')+decodes,8))[0]==before
                # Real guest atomic writes, then real input opens Start to request refresh.
                q.type('run /bin/update.elf\n',pause=1)
                start()
                for _ in range(80):
                    time.sleep(.1); data=catalog()
                    if b'Updated Gallery\0' in data: break
                else: raise AssertionError('Live app metadata did not refresh on Start open')
                count=struct.unpack_from('<I',data)[0]
                gallery=gallery_index(data,b'Updated Gallery\0')
                icon_pointer=pointer(data,gallery)
                assert struct.unpack('<I',memory(icon_pointer,4))[0]==0xff0000ff
                after=struct.unpack('<Q',memory(address('g_resource_store')+decodes,8))[0]
                assert after==before+1,(before,after)
                assert struct.unpack('<Q',memory(address('g_resource_store')+hits,8))[0]>0
                q.capture(label+'-updated')
                # Launch the manifest-backed entry through the actual menu.
                before_windows=struct.unpack('<I',memory(address('g_window_count'),4))[0]
                bar=struct.unpack('<I',memory(address('g_taskbar_h'),4))[0]
                row=max(26,768*38//1080); header=max(24,768*36//1080); pad=max(6,768*10//1080)
                panel_y=768-bar-(header+count*row+pad*2)-768*4//1080
                target_y=panel_y+header+pad+row//2
                move(20,target_y)
                q.monitor('mouse_button 1'); time.sleep(.2); q.monitor('mouse_button 0')
                for _ in range(50):
                    time.sleep(.1)
                    if struct.unpack('<I',memory(address('g_window_count'),4))[0]>before_windows: break
                else: raise AssertionError('Manifest app did not launch from Start')
                for _ in range(100):
                    time.sleep(.05); data=catalog()
                    if pointer(data,gallery,title) and pointer(data,gallery,task): break
                else: raise AssertionError('Visible app window did not request title/task icons')
                assert not pointer(data,gallery)
                q.capture(label+'-launched')
                q.monitor('sendkey ctrl-c')
                for _ in range(100):
                    time.sleep(.05); data=catalog()
                    if not pointer(data,gallery,title) and not pointer(data,gallery,task): break
                else: raise AssertionError('Closed app retained scaled window icons')
                q.type('write /tmp/lifecycle alive\n',pause=.2)
                q.type('cat /run/boot.log\n',pause=.2)
                print(label+': wallpaper, lazy menu/window icons, live replacement, release and runtime mounts passed.',flush=True)
            finally: q.close()

        put('/etc/desktop.cfg',b'/home/wall.png\n'); boot('image-png')
        # Reset runtime-mutated files in the offline seed before the next scenario.
        put('/etc/desktop.cfg',b'/home/wall.jpg\n'); boot('image-jpeg')
        put('/home/wall.png',b'\x89PNG\r\n\x1a\ncorrupt')
        put('/etc/desktop.cfg',b'/home/wall.png\n'); boot('image-corrupt',wallpaper_ok=False)

        def recovery_boot(label,storage):
            if storage:
                with base.open('rb') as source, smoke.IMAGE.open('wb') as target:
                    copy_sparse(source,target,offset)
                    with partition.open('rb') as volume: copy_sparse(volume,target,partition.stat().st_size)
            smoke.BOOT_ISO=not storage
            q=smoke.Qemu(storage=storage,resolution=(1024,768))
            state=smoke.OUT/'image-recovery-state.bin'
            def memory(addr,size):
                assert not q.monitor(f'memsave 0x{addr:x} {size} "{state}"')
                return state.read_bytes()
            try:
                for _ in range(120):
                    time.sleep(.5)
                    try: log=q.capture(label)
                    except AssertionError: continue
                    if memory(address('g_terminal_shell_ready'),1)==b'\1': break
                else: raise AssertionError(log[-4000:])
                assert memory(address('g_recovery_retired'),1)==b'\0'
                assert memory(address('g_recovery_heap_bytes'),8)==bytes(8)
                assert memory(address('g_recovery_archive_bytes'),8)==bytes(8)
                assert memory(address('g_recovery_fallback_bytes'),8)==bytes(8)
                assert memory(address('ramfs_root_node'),8)!=bytes(8)
                if storage: assert '[storage] Disk init failed; RAM recovery mode.' in log
                for _ in range(100):
                    pointer=struct.unpack('<Q',memory(address('g_resources_catalog'),8))[0]
                    data=memory(pointer,total)
                    if struct.unpack_from('<I',data)[0]==2: break
                    time.sleep(.05)
                else: raise AssertionError('Recovery catalog did not replace disk catalog')
                assert not struct.unpack_from('<Q',data,apps+icons+pixels)[0]
                q.type('run /bin/win_test.elf\n',pause=.5)
                q.monitor('sendkey ctrl-c'); time.sleep(.5)
                q.capture(label+'-usable')
                print(label+': recovery retained, lazy app icons and RAM apps remain usable.',flush=True)
            finally:
                q.close(); smoke.BOOT_ISO=False
        recovery_boot('image-recovery',storage=False)
        put('/bin/init.elf',b'not a valid ELF')
        recovery_boot('image-invalid-disk-shell',storage=True)


if __name__=='__main__': main()
