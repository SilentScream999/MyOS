#!/usr/bin/env python3
"""Real GUI decode/DMA, pause past the service deadline, picker and cleanup.

Uses a diskless guest and a private ISO; run after make iso. Requires the same
QEMU/OVMF/libmp3lame dependencies as qemu_mp3_test.py.
"""
import subprocess
import time
import qemu_storage_smoke as smoke
from qemu_audio_test import boot
from qemu_mp3_test import fixtures
from audio_service_probe import AudioServiceProbe
from run_explorer_tests import save_png


def main():
    smoke.OUT.mkdir(exist_ok=True)
    source=smoke.OUT/'music-launch.cpp';obj=source.with_suffix('.o');binary=source.with_suffix('.elf')
    source.write_text(r'''
static long call(unsigned long n,unsigned long a=0,unsigned long b=0,unsigned long c=0){
    long result;asm volatile("syscall":"=a"(result):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return result;
}
extern "C" void _start(){
    long pid=call(216,(unsigned long)"/bin/music_player.wiz",(unsigned long)"/long.mp3");
    call(60,pid>0?0:1);for(;;){}
}
''')
    subprocess.run(['clang++','-target','x86_64-elf','-ffreestanding','-fno-stack-protector',
                    '-fno-pic','-fno-pie','-mno-red-zone','-c',str(source),'-o',str(obj)],check=True)
    subprocess.run(['ld.lld','-nostdlib','-z','max-page-size=0x1000','-Ttext=0x400000',
                    '-o',str(binary),str(obj)],check=True)
    smoke.BOOT_ISO=True;smoke.ISO_PATH=fixtures({'music-launch.elf':binary.read_bytes()})
    q=smoke.Qemu(storage=False,input_devices=True,extra_args=(
        '-audiodev','none,id=audio','-device','intel-hda','-device','hda-duplex,audiodev=audio,cad=2'))
    try:
        boot(q,'music-player-boot');probe=AudioServiceProbe(q)
        def wait(check,message):
            deadline=time.monotonic()+15
            while time.monotonic()<deadline:
                if check():return
                time.sleep(.1)
            raise AssertionError(message+'\n'+q.capture('music-player-failure')[-4000:])
        def key(code):
            assert not q.monitor('sendkey '+code+' 60');time.sleep(.2)
        def consumed():return probe.read(probe.symbols['hda::pcm']+8,8)
        def move(x,y):
            for _ in range(80):
                dx=x-probe.number('g_mouse_x',4);dy=y-probe.number('g_mouse_y',4)
                if abs(dx)<=1 and abs(dy)<=1:break
                q.monitor(f'mouse_move {max(-80,min(80,dx))} {max(-80,min(80,dy))}');time.sleep(.025)
            else:raise AssertionError('Mouse did not reach its target')
        def close_window():
            move(800,85)
            q.monitor('mouse_button 1');time.sleep(.08);q.monitor('mouse_button 0')
        q.type('run /music-launch.elf\n',pause=0)
        wait(lambda:probe.number('hda::pcm_running',1),'GUI did not start PCM')
        time.sleep(.3)
        wait(lambda:consumed()>0,'DMA did not consume GUI samples')
        key('spc');wait(lambda:probe.number('hda::pcm_paused',1),'Space did not pause')
        before=consumed();probe.assert_idle(1.4)
        assert consumed()==before,'Paused DMA consumed samples'
        master=probe.read(probe.symbols['g_audio_mixer'],16)
        move(522,530);q.monitor('mouse_button 1');time.sleep(.1)
        move(470,530);time.sleep(.1)
        wait(lambda:15<=probe.number('hda::pcm_percent',4)<=25,'Dragging did not adjust app gain')
        move(574,530);time.sleep(.1);q.monitor('mouse_button 0')
        wait(lambda:75<=probe.number('hda::pcm_percent',4)<=85,'Held drag did not follow pointer')
        wait(lambda:probe.number('hda::pcm_gain_applied')==probe.number('hda::pcm_gain_revision'),'App gain did not reach the codec')
        key('m');wait(lambda:probe.number('hda::pcm_muted',1),'App mute was not applied')
        wait(lambda:probe.number('hda::pcm_gain_applied')==probe.number('hda::pcm_gain_revision'),'App mute did not reach the codec')
        assert probe.read(probe.symbols['g_audio_mixer'],16)==master,'App controls changed the system mixer'
        key('m');probe.assert_idle()
        # A modal file browser must still work while DMA ownership is paused.
        key('ctrl-o');time.sleep(.3);key('esc')
        assert probe.number('hda::pcm_paused',1)
        q.command('screendump',{'filename':str(smoke.OUT/'music-player-paused.ppm')})
        save_png(smoke.OUT/'music-player-paused.ppm',smoke.OUT/'music-player-paused.png')
        key('spc');wait(lambda:not probe.number('hda::pcm_paused',1),'Resume failed')
        wait(lambda:consumed()>before,'Resume did not preserve/advance DMA position')
        key('s');wait(lambda:not probe.number('hda::pcm_owner',4),'Stop retained output ownership')
        probe.assert_idle()
        key('spc');wait(lambda:probe.number('hda::pcm_running',1),'Replay failed')
        # A short replacement exercises EOF below the normal prefill threshold.
        key('ctrl-o');q.type('/short.mp3\n',pause=0)
        wait(lambda:not probe.number('hda::pcm_owner',4),'Short song did not finish')
        assert q.capture('music-player-short').count('Audio: PCM stream opened')==3
        key('ctrl-o');q.type('/long.mp3\n',pause=0)
        wait(lambda:probe.number('hda::pcm_running',1),'Long replacement failed')
        close_window();wait(lambda:not probe.number('hda::pcm_owner',4),'Window close retained output ownership')
        probe.assert_idle()
        log=q.capture('music-player-complete')
        assert not any(error in log for error in ('PCM underrun','PCM DMA stalled','PCM service deadline missed',
                                                   'PCM descriptor DMA error','PCM hardware FIFO error')),log[-4000:]
        assert q.command('query-status')['running']
    finally:q.close()
    print('PASS: native music GUI playback, pause/resume, idle worker, picker, stop/replay, short EOF and close')


if __name__=='__main__':main()
