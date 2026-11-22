"""Exercise the standalone scaling-test app through the userspace entry point.

QEMU checks window creation, input and resize. Intel execution needs hardware.
"""
import struct
import subprocess
import time
import qemu_storage_smoke as smoke
from qemu_debug_memory import DebugMemory


def main():
    symbols=subprocess.check_output(['nm','-n','-C','wyzar'],text=True).splitlines()
    def address(name):
        return next(int(line.split()[0],16) for line in symbols if line.split()[-1]==name)
    smoke.BOOT_ISO=True
    q=smoke.Qemu(storage=False,resolution=(1280,800),
                 extra_args=('-no-shutdown','-gdb','tcp:127.0.0.1:45456'))
    debug=None
    try:
        for _ in range(160):
            time.sleep(.5)
            try:log=q.capture('gpu-window-boot')
            except AssertionError:continue
            if '[wm] Shell task started inside WM frame.' in log:break
        else:raise AssertionError('Shell did not boot')
        q.type('run /bin/gpu_test.elf\n',pause=.5)
        time.sleep(.5)
        def stop():
            nonlocal debug
            q.command('stop');debug=DebugMemory()
        def resume():
            nonlocal debug
            debug.close();debug=None;q.command('cont')
        def value(name,size=8):
            return int.from_bytes(debug.read(address(name),size),'little')
        def move_to(tx,ty):
            for _ in range(80):
                stop();mx=value('g_mouse_x',4);my=value('g_mouse_y',4);resume()
                dx=tx-mx;dy=ty-my
                if not dx and not dy:return
                q.monitor(f'mouse_move {max(-80,min(80,dx))} {max(-80,min(80,dy))}')
                time.sleep(.04)
            raise AssertionError('Pointer target unreachable')
        stop()
        count=value('g_window_count',4)
        windows=struct.unpack('<'+'Q'*count,debug.read(address('g_window_list'),count*8))
        window=next(w for w in windows if debug.read(w+4,64).split(b'\0')[0]==b'GPU Scaling Test')
        # Window's leading fields are id, title[64], executable[256], x/y/w/h.
        x,y,w,h=struct.unpack('<iiii',debug.read(window+324,16))
        assert (w,h)==(320,240)
        pixels=int.from_bytes(debug.read(window+344,8),'little')
        def sample(px,py):
            return int.from_bytes(debug.read(pixels+(py*320+px)*4,4),'little')
        assert sample(40,40)==0xFFDE3C4B
        resume();move_to(x+16,y+16)
        q.monitor('mouse_button 1');time.sleep(.15);q.monitor('mouse_button 0')
        q.type('1',pause=.2);time.sleep(.3);stop()
        assert sample(40,40)==0xFF235789
        resume();q.type('3',pause=.2);time.sleep(.3);stop()
        assert sample(40,40)!=0xFF235789
        resume();q.type('1 ',pause=.2);time.sleep(.3);stop()
        before=debug.read(pixels+70*320*4,320*4)
        resume();time.sleep(.2);stop()
        assert debug.read(pixels+70*320*4,320*4)!=before
        resume();q.type(' ',pause=.2);time.sleep(.3);stop()
        still=debug.read(pixels+70*320*4,320*4)
        resume();time.sleep(.2);stop()
        assert debug.read(pixels+70*320*4,320*4)==still
        resume()
        move_to(x+w-2,y+h-2)
        q.monitor('mouse_button 1');time.sleep(.15)
        move_to(x+w+135,y+h+89)
        q.monitor('mouse_button 0');time.sleep(.4);stop()
        rw,rh=struct.unpack('<ii',debug.read(window+332,8))
        assert rw>w and rh>h
        title=value('g_title_h',4)
        pad=max(2,title//10);button=min(title+8,(rw-2*pad-4)//3)
        resume();move_to(x+rw-pad-button//2,y-title//2)
        q.monitor('mouse_button 1');time.sleep(.15);q.monitor('mouse_button 0')
        time.sleep(.4);stop()
        assert value('g_window_count',4)==count-1
        print('GPU test window launch, patterns, animation, pause, resize and close passed.',flush=True)
    finally:
        if debug:debug.close()
        q.close()


if __name__=='__main__':
    main()
