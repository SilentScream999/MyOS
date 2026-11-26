#!/usr/bin/env python3
"""Production CPU popup input/rendering and PS/2 hold-repeat; no disks attached."""
import subprocess
import time
import qemu_storage_smoke as smoke
from qemu_audio_test import boot
from audio_service_probe import AudioServiceProbe

def main():
    smoke.OUT.mkdir(exist_ok=True);smoke.BOOT_ISO=True
    symbols=subprocess.check_output(['nm','-n','-C','wyzar'],text=True).splitlines()
    def address(name):return next(int(line.split()[0],16) for line in symbols if line.split()[-1]==name)
    q=smoke.Qemu(storage=False,resolution=(1280,800),input_devices=False,extra_args=('-device','usb-mouse','-no-shutdown'))
    def memory(name,length=4):
        path=smoke.OUT/'volume-state.bin';q.command('stop')
        try:
            assert not q.monitor(f'memsave 0x{address(name):x} {length} "{path}"')
            return path.read_bytes()
        finally:q.command('cont')
    def scalar(name):return int.from_bytes(memory(name),'little')
    def state():
        data=memory('g_audio_mixer',8)
        return int.from_bytes(data[:4],'little'),bool(data[4])
    def move(x,y):
        for _ in range(80):
            dx=x-scalar('g_mouse_x');dy=y-scalar('g_mouse_y')
            if abs(dx)<=1 and abs(dy)<=1:return
            q.monitor(f'mouse_move {max(-80,min(80,dx))} {max(-80,min(80,dy))}');time.sleep(.02)
        raise AssertionError('Pointer did not move')
    def click(x,y):
        move(x,y);q.monitor('mouse_button 1');time.sleep(.08);q.monitor('mouse_button 0');time.sleep(.2)
    def key(code,duration=60):
        q.command('send-key',{'keys':[{'type':'qcode','data':code}],'hold-time':duration})
        time.sleep(duration/1000+.2)
    try:
        boot(q,'volume-boot');assert state()==(70,False)
        service=AudioServiceProbe(q);service.assert_idle()
        for code in ('f1','f2','f3'):
            key(code,500);assert state()==(70,False),'Ordinary function key changed volume/mute'
        tb=scalar('g_taskbar_h')
        for x in range(1260,940,-10):
            click(x,800-tb//2)
            if scalar('g_taskbar_popup')==3:break
        else:raise AssertionError('Volume tray icon did not open popup')
        anchor=scalar('g_taskbar_anchor');x=min(anchor,1280-326);y=800-tb-124-4
        left=x+64;right=x+320-24;cy=y+124-34
        click(left,cy);assert state()==(0,False)
        click(right,cy);assert state()==(100,False)
        click((left+right)//2,cy);assert state()==(50,False)
        # Drag capture survives pointer motion outside the popup and clamps.
        move(left+10,cy);q.monitor('mouse_button 1');time.sleep(.1)
        move(1279,cy);q.monitor('mouse_button 0');time.sleep(.2)
        assert state()==(100,False) and not scalar('g_volume_drag')
        click(x+28,cy);assert state()==(100,True)
        move(950,600);time.sleep(.3)
        q.command('screendump',{'filename':str(smoke.OUT/'volume-popup-cpu.ppm')})
        muted_pixels=(smoke.OUT/'volume-popup-cpu.ppm').read_bytes().split(b'\n',3)[3]
        key('volumeup');assert state()==(100,False)
        for _ in range(20):
            q.command('screendump',{'filename':str(smoke.OUT/'volume-popup-unmuted.ppm')})
            unmuted=(smoke.OUT/'volume-popup-unmuted.ppm').read_bytes().split(b'\n',3)[3]
            # No pointer movement: the shared-state notification must repaint
            # the popup's mute icon/label after a keyboard-only change.
            if any(muted_pixels[(row*1280+x)*3:(row*1280+x+320)*3]!=unmuted[(row*1280+x)*3:(row*1280+x+320)*3] for row in range(y,y+124)):break
            time.sleep(.1)
        else:raise AssertionError('Keyboard change left stale popup pixels')
        key('volumedown',1000);assert state()[0]<=30,state() # Several repeats, allowing emulator scheduling jitter.
        stopped=state();time.sleep(.3);assert state()==stopped,'Repeat continued after key release'
        key('audiomute',1000);assert state()[1],'Holding mute toggled repeatedly'
        key('esc')
        for _ in range(20):
            if scalar('g_taskbar_popup')==0:break
            time.sleep(.1)
        assert scalar('g_taskbar_popup')==0,('Escape did not close popup',scalar('g_kbd_modifiers'))
        click(anchor+tb//2,800-tb//2);assert scalar('g_taskbar_popup')==3
        click(950,600);assert scalar('g_taskbar_popup')==0
        service.assert_idle()
        assert q.command('query-status')['running']
        print('PASS: CPU tray/popup, 0/50/100 slider, drag capture/clamping, mute/shared keys, hold repeat/release, Escape and outside dismissal')
    finally:q.close()

if __name__=='__main__':main()
