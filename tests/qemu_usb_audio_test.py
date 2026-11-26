#!/usr/bin/env python3
"""Real xHCI boot keyboard media usages, multiple devices and hub hotplug.

QEMU usb-kbd exposes Keyboard-page media usages. Consumer descriptor setup,
bitmaps, arrays, Report IDs and independent repeat state are covered by the
host HID media/setup tests; this does not emulate the physical Dareu layout.
"""
import subprocess
import time
import qemu_storage_smoke as smoke
from qemu_audio_test import boot
from audio_service_probe import AudioServiceProbe


def main():
    smoke.OUT.mkdir(exist_ok=True);smoke.BOOT_ISO=True
    symbols=subprocess.check_output(['nm','-n','-C','wyzar'],text=True).splitlines()
    def address(name):return next(int(line.split()[0],16) for line in symbols if line.split()[-1]==name)
    q=smoke.Qemu(storage=False,input_devices=False,extra_args=(
        '-no-shutdown','-audiodev','none,id=audio','-device','intel-hda',
        '-device','hda-duplex,audiodev=audio,cad=2','-device','usb-hub,id=mediahub,bus=xhci.0,port=1',
        '-device','usb-kbd,id=directkbd,bus=xhci.0,port=2',
        '-device','usb-kbd,id=hubkbd,bus=xhci.0,port=1.1'))
    def memory(name,size):
        path=smoke.OUT/'usb-audio-state.bin';q.command('stop')
        try:assert not q.monitor(f'memsave 0x{address(name):x} {size} "{path}"')
        finally:q.command('cont')
        return path.read_bytes()
    def number(name,size=4):return int.from_bytes(memory(name,size),'little')
    def state():
        data=memory('g_audio_mixer',8)
        return int.from_bytes(data[:4],'little'),bool(data[4])
    def key(code,duration=60):
        # Explicit release: QMP send-key's timer uses guest virtual time,
        # which can lag wall time under TCG and make a hold appear stuck.
        for down in (True,False):
            q.command('input-send-event',{'events':[{'type':'key','data':{
                'down':down,'key':{'type':'qcode','data':code}}}]})
            time.sleep(duration/1000 if down else .25)
    def keyboards(count):
        for _ in range(80):
            if number('keyboard_count')==count:return
            time.sleep(.1)
        raise AssertionError((count,number('keyboard_count'),q.capture('usb-audio-count')[-3000:]))
    try:
        boot(q,'usb-audio-boot');keyboards(2);assert state()==(70,False)
        service=AudioServiceProbe(q);service.assert_idle(2)
        for code in ('f1','f2','f3'):key(code)
        assert state()==(70,False)
        service.assert_idle()
        key('volumeup');assert state()==(80,False),state()
        assert number('g_usb_media_key_events',8)>0,'Volume used PS/2 rather than USB'
        key('volumedown');assert state()==(70,False)
        key('audiomute',1000);assert state()==(70,True),'Mute repeated while held'
        key('volumedown',1000);assert state()[0]<=40 and not state()[1],state() # At least 3 steps despite TCG jitter.
        stopped=state();time.sleep(.4);assert state()==stopped,'Repeat outlived USB release'
        service.assert_idle()
        q.command('device_del',{'id':'hubkbd'});keyboards(1)
        before=state()[0];key('volumeup');assert state()==(before+10,False)
        q.command('device_add',{'driver':'usb-kbd','id':'replacement','bus':'xhci.0','port':'1.1'})
        keyboards(2);before=state()[0];key('volumeup');assert state()==(before+10,False)
        # Whichever QEMU keyboard currently receives input, detaching all USB
        # sources while a key is held must stop their source-local repeat.
        q.command('input-send-event',{'events':[{'type':'key','data':{
            'down':True,'key':{'type':'qcode','data':'volumedown'}}}]})
        time.sleep(.15)
        q.command('device_del',{'id':'replacement'});q.command('device_del',{'id':'directkbd'})
        keyboards(0)
        q.command('input-send-event',{'events':[{'type':'key','data':{
            'down':False,'key':{'type':'qcode','data':'volumedown'}}}]})
        stopped=state();time.sleep(.7);assert state()==stopped
        service.assert_idle()
        assert q.command('query-status')['running']
        print('PASS: zero idle audio iterations/CPU cycles, actual USB media keys, plain F1-F3 preservation, hold/release, two keyboards, direct/hub hotplug and disconnect')
    finally:q.close()


if __name__=='__main__':main()
