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
        def window_list():
            count=value('g_window_count',4)
            return struct.unpack('<'+'Q'*count,debug.read(address('g_window_list'),count*8))
        # Kernel shell creation precedes the private console's input window.
        # Wait for both instead of sending keys to an unfocused boot window.
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            stop()
            focused=value('g_focused_win_id',4)
            ready=value('g_terminal_shell_ready',1) and any(
                int.from_bytes(debug.read(w,4),'little')==focused and
                debug.read(w+4,64).split(b'\0')[0]==b'Terminal'
                for w in window_list())
            resume()
            if ready:break
            time.sleep(.1)
        else:raise AssertionError('Primary console did not become ready for input')
        q.type('run /bin/gpu_test.elf\n',pause=.5)
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            stop();windows=window_list();count=len(windows)
            window=next((w for w in windows if debug.read(w+4,64).split(b'\0')[0]==b'GPU Scaling Test'),0)
            if window:
                backing=int.from_bytes(debug.read(window+344,8),'little')
                if backing and int.from_bytes(debug.read(backing+(40*320+40)*4,4),'little')==0xFFDE3C4B:
                    break
            resume();time.sleep(.1)
        else:raise AssertionError('GPU test did not create and paint its window')
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
        q.capture('gpu-titlebar-preview')
        resume();q.type('a',pause=.2);time.sleep(.3);stop()
        assert sample(40,40)==0,'Transparent test band was not premultiplied'
        assert sample(160,40)==0x80122C45,'Half-alpha test band has incorrect premultiplied RGB'
        assert sample(260,40)==0xFF235789
        resume();q.type('a',pause=.2);time.sleep(.3);stop()
        assert sample(40,40)==0xFF235789,'Opaque mode did not restore legacy pixels'
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
        taskbar=value('g_taskbar_h',4)
        resume();move_to(45,800-taskbar//2)
        q.monitor('mouse_button 1');time.sleep(.15);q.monitor('mouse_button 0')
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            time.sleep(.1);stop()
            cache,capacity,key=struct.unpack('<QQQ',debug.read(address('g_menu_pixel_cache'),24))
            if value('g_menu_open',1) and cache and capacity>=1280*800*4 and key:break
            resume()
        else:raise AssertionError('Start menu did not build its pixel cache')
        catalog=value('g_resources_catalog')
        app_count=int.from_bytes(debug.read(catalog,4),'little');assert app_count>0
        header=max(44,800*56//1080);row=max(26,800*38//1080);pad=max(10,800*14//1080)
        height=min(header+max(1,app_count)*row+2*pad+max(52,800*64//1080),800*740//1080,800-taskbar-12)
        top=max(0,800-taskbar-height-800*4//1080)
        hover_x=6+pad+2;hover_y=top+header+pad+row//2
        offset=(hover_y*1280+hover_x)*4
        original=debug.read(cache+offset,4)
        resume();move_to(hover_x,hover_y)
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            time.sleep(.1);stop()
            cache,capacity,new_key=struct.unpack('<QQQ',debug.read(address('g_menu_pixel_cache'),24))
            if new_key!=key and debug.read(cache+offset,4)!=original:break
            resume()
        else:raise AssertionError('Start hover did not refresh cached menu pixels')
        # Leave the highlighted item without closing Start. Both its old
        # color and the original cache key must return.
        resume();move_to(45,800-taskbar//2)
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            time.sleep(.1);stop()
            cache,capacity,new_key=struct.unpack('<QQQ',debug.read(address('g_menu_pixel_cache'),24))
            if new_key==key and debug.read(cache+offset,4)==original:break
            resume()
        else:raise AssertionError('Leaving the item did not clear its hover highlight')
        resume();q.monitor('sendkey esc');time.sleep(.3);stop()
        assert not value('g_menu_open',1),'Escape did not close Start'
        print('Start open, cached hover pixels and Escape close passed on CPU fallback.',flush=True)
        print('GPU test window launch, patterns, animation, alpha toggle, pause, resize and close passed.',flush=True)
    finally:
        if debug:debug.close()
        q.close()


if __name__=='__main__':
    main()
