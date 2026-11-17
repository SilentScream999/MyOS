#!/usr/bin/env python3
"""Explorer image launch, PNG/JPEG viewing, dialogs, zoom and warm Start cache.

Build: make iso PERSIST_DISK=.storage-test/viewer-seed.img ROOT_MIB=64
Run: python3 tests/qemu_image_viewer_test.py
"""
from pathlib import Path
import struct
import subprocess
import sys
import time
import qemu_storage_smoke as smoke
from run_explorer_tests import save_png
from run_image_tests import fixtures, OUT as FIXTURES

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/'.storage-test'
sys.path.insert(0, str(ROOT/'tools'))
from image_io import copy_sparse

def main():
    fixtures()
    seed=OUT/'viewer-seed.img';image=OUT/'viewer-qemu.img';partition=OUT/'viewer-partition.img'
    with seed.open('rb') as source,image.open('wb') as target:
        copy_sparse(source,target,seed.stat().st_size)
    with image.open('rb') as source,partition.open('wb') as target:
        mbr=source.read(512);start,sectors=struct.unpack_from('<II',mbr,470)
        source.seek(start*512);copy_sparse(source,target,sectors*512)
    bad=OUT/'viewer-broken.jpg';bad.write_bytes(b'not an image')
    manifest=OUT/'viewer-files.txt'
    manifest.write_text(f'mkdir /home/viewer-test\nput /home/viewer-test/a.png {FIXTURES}/blue.png\n'
                        f'put /home/viewer-test/b.jpg {ROOT}/tests/fixtures/images/testorig.jpg\n'
                        f'put /home/viewer-test/broken.jpg {bad}\n')
    subprocess.run([str(OUT/'ext4-image-tool'),str(partition),str(manifest)],check=True)
    with image.open('r+b') as target,partition.open('rb') as source:
        target.seek(start*512);copy_sparse(source,target,sectors*512)
    symbols=subprocess.check_output(['nm','-n','-C',str(ROOT/'wyzar')],text=True).splitlines()
    def address(name):return next(int(l.split()[0],16) for l in symbols if l.split()[-1]==name)
    smoke.IMAGE=image;smoke.BOOT_ISO=True
    q=smoke.Qemu(resolution=(1280,800))
    try:
        def memory(addr,size):
            path=OUT/'viewer-state.bin';q.command('stop')
            try:
                assert not q.monitor(f'memsave 0x{addr:x} {size} "{path}"')
                return path.read_bytes()
            finally:q.command('cont')
        def scalar(name,size=4):return int.from_bytes(memory(address(name),size),'little')
        def free_ram():
            free=scalar('pmm_free_count',8)*4096
            current=scalar('pmm_cur_region');count=scalar('pmm_region_count')
            regions=list(struct.iter_unpack('<QQ',memory(address('pmm_regions'),count*16)))
            return free+sum(end-bump for bump,end in regions[current:])
        def windows():
            count=scalar('g_window_count');result={}
            for (p,) in struct.iter_unpack('<Q',memory(address('g_window_list'),count*8)):
                data=memory(p,384);result[data[4:68].split(b'\0')[0].decode()]=(p,struct.unpack_from('<iiii',data,324))
            return result
        def pixels(title='Image Viewer'):
            p,(_,_,w,h)=windows()[title];buffer=struct.unpack_from('<Q',memory(p,384),344)[0]
            return memory(buffer,w*h*4)
        def solid_rows(title,rows,left,right,color):
            _,(_,_,w,h)=windows()[title];data=pixels(title)
            for y in rows:
                for x in range(left,right):
                    actual=struct.unpack_from('<I',data,(y*w+x)*4)[0]
                    assert actual==color,(title,'fragmented control',x,y,hex(actual),hex(color))
        def visible_icon(title,x,y,w,h):
            _,(_,_,stride,_)=windows()[title];data=pixels(title)
            bright=sum((struct.unpack_from('<I',data,((y+py)*stride+x+px)*4)[0]&0xffffff)>0xb0b0b0
                       for py in range(6,h-6) for px in range(6,w-6))
            assert bright>10,(title,'missing navigation icon',x,y)
        def move(x,y):
            for _ in range(80):
                dx=x-scalar('g_mouse_x');dy=y-scalar('g_mouse_y')
                if abs(dx)<=1 and abs(dy)<=1:return
                q.monitor(f'mouse_move {max(-80,min(80,dx))} {max(-80,min(80,dy))}');time.sleep(.025)
            raise AssertionError('Pointer target not reached')
        def click(x,y):
            move(x,y);q.monitor('mouse_button 1');time.sleep(.07);q.monitor('mouse_button 0');time.sleep(.2)
        def capture(label):
            q.command('screendump',{'filename':str(OUT/(label+'.ppm'))})
            save_png(OUT/(label+'.ppm'),OUT/(label+'.png'))
        def wait_for(check,message):
            deadline=time.monotonic()+15
            while time.monotonic()<deadline:
                if check():return
                time.sleep(.15)
            raise AssertionError(message)
        for _ in range(100):
            time.sleep(.5)
            try:log=q.capture('viewer-boot')
            except AssertionError:continue
            if '[wm] Shell task started inside WM frame.' in log:break
        else:raise AssertionError('boot timeout')
        q.type('run /bin/explorer.wiz\n',pause=1)
        wait_for(lambda:'File Explorer' in windows(),'Explorer not launched')
        time.sleep(.3)
        visible_icon('File Explorer',84,12,32,30)
        visible_icon('File Explorer',120,12,32,30)
        capture('explorer-navigation-icons')
        _,(x,y,_,_)=windows()['File Explorer'];click(x+300,y+26)
        q.type('/home/viewer-test\n',pause=.5)
        click(x+260,y+150);click(x+260,y+150)
        wait_for(lambda:'Image Viewer' in windows(),'PNG association did not launch viewer')
        time.sleep(.5);data=pixels()
        blue=struct.pack('<I',0xff0000ff)
        assert sum(p==0xff0000ff for (p,) in struct.iter_unpack('<I',data))>100000,'PNG not rendered in Fit'
        capture('viewer-png-fit')
        # Flat bands avoid the glyph and antialiased corners. They must remain
        # solid even when another task's SSE compositor preempts a repaint.
        for _ in range(12):
            q.monitor('sendkey equal 40');time.sleep(.2)
            solid_rows('Image Viewer',(15,37),158,194,0xff2c273b)
        q.monitor('sendkey f');time.sleep(.2)
        q.monitor('sendkey 1');time.sleep(.4)
        assert sum(p==0xff0000ff for (p,) in struct.iter_unpack('<I',pixels()))==2,'Actual size is not 2x1'
        q.monitor('sendkey f');time.sleep(.2)
        def open_file(name):
            q.monitor('sendkey ctrl-o');wait_for(lambda:'Open' in windows(),'Open dialog missing')
            time.sleep(.3)
            pointer,_=windows()['Open']
            assert scalar('g_focused_win_id')==int.from_bytes(memory(pointer,4),'little'),'Picker did not receive focus'
            _,(_,_,w,h)=windows()['Open']
            visible_icon('Open',12,14,36,30)
            visible_icon('Open',56,14,36,30)
            solid_rows('Open',(h-94,h-74),252,w-24,0xff443454)
            # Verify the buttons in the constructor-free native app, not just
            # in a hosted preview. Refresh keeps the list; Up changes it.
            _,(px,py,_,_)=windows()['Open']
            first=pixels('Open')[96*w*4:(h-106)*w*4]
            click(px+74,py+28)
            assert pixels('Open')[96*w*4:(h-106)*w*4]==first,'Refresh changed the folder listing'
            click(px+30,py+28)
            assert pixels('Open')[96*w*4:(h-106)*w*4]!=first,'Up did not navigate to the parent'
            capture('picker-navigation-icons')
            q.monitor('sendkey ctrl-l');time.sleep(.2);q.type('/home/viewer-test\n',pause=.3)
            click(px+300,py+h-85)
            q.monitor('sendkey ctrl-a');time.sleep(.2);q.type('/home/viewer-test/'+name+'\n',pause=.7)
            capture('viewer-open-'+name.replace('.','-'))
        open_file('b.jpg');wait_for(lambda:'Open' not in windows(),'JPEG was not accepted')
        assert pixels()!=data,'JPEG did not replace PNG';capture('viewer-jpeg')
        before=pixels();open_file('broken.jpg');assert 'Open' in windows(),'Invalid image closed picker'
        # Failed load preserves the image viewport; the status message changes.
        assert pixels()[800*52*4:800*(560-28)*4]==before[800*52*4:800*(560-28)*4]
        q.monitor('sendkey esc');time.sleep(.3);assert 'Open' not in windows()
        _,(x,y,w,h)=windows()['Image Viewer'];title=scalar('g_title_h');pad=max(2,title//10);bw=title+8
        click(x+w-pad-2*bw-2+bw//2,y-title//2);time.sleep(.3)
        assert windows()['Image Viewer'][1][2]==1280,'Maximize/resize failed';capture('viewer-maximized')
        click(30,780);time.sleep(.4);stamp=scalar('g_resource_refresh_ticks',8)
        click(30,780);click(30,780);time.sleep(.3)
        assert scalar('g_resource_refresh_ticks',8)==stamp,'Rapid Start reopening refreshed disk resources'
        click(30,780)
        def close_viewer():
            _,(x,y,w,h)=windows()['Image Viewer']
            click(x+w-pad-bw//2,y-title//2)
            wait_for(lambda:'Image Viewer' not in windows(),'Viewer did not close')
            time.sleep(1)
        close_viewer();baseline=free_ram()
        for cycle in range(3):
            _,(x,y,_,_)=windows()['File Explorer']
            click(x+260,y+150);click(x+260,y+150)
            wait_for(lambda:'Image Viewer' in windows(),'Viewer did not reopen')
            time.sleep(.4);opened=free_ram()
            assert baseline-opened>48*1024*1024,'Viewer arena was not allocated'
            close_viewer();closed=free_ram()
            print(f'Viewer RAM cycle {cycle+1}: allocated {(baseline-opened)//1024} KiB, residual {(baseline-closed)//1024} KiB',flush=True)
            assert baseline-closed<=256*1024,'Viewer leaked RAM after close'
        print('QEMU Image Viewer passed: Explorer launch, PNG/JPEG, actual size, failed-load preservation, dialogs, resize, warm Start cache and repeated exit memory reclamation.',flush=True)
    finally:q.close()

if __name__=='__main__':
    main()
