#!/usr/bin/env python3
"""Measure rainbow work under partial/full occlusion and test real dragging.

--baseline records old behavior without the optimization-specific assertions.
QEMU snapshot mode keeps the private fixture unchanged. Prepare it with:
  python3 tools/make_usb_image.py --output .storage-test/rainbow-base.img --debug
Use --update-boot --debug to refresh an existing fixture after a kernel build.
"""
from pathlib import Path
import argparse
import struct
import subprocess
import tempfile
import time
import qemu_storage_smoke as smoke

ROOT=Path(__file__).resolve().parents[1]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline',action='store_true')
    parser.add_argument('--image',type=Path,default=ROOT/'.storage-test/rainbow-base.img')
    args=parser.parse_args()
    symbols=subprocess.check_output(['nm','-n','-C',str(ROOT/'wyzar')],text=True).splitlines()
    def address(name):
        return next(int(line.split()[0],16) for line in symbols if line.split()[-1]==name)
    usage=next(int(line.split()[0],16) for line in symbols if line.endswith('::usage'))
    source=(ROOT/'src/gui/wm.h').read_text()
    event='struct WinEvent {'+source.split('struct WinEvent {',1)[1].split('\n};',1)[0]+'\n};'
    window='struct Window {'+source.split('struct Window {',1)[1].split('\n};',1)[0]+'\n};'
    window=window.replace('pid_t','uint32_t')
    with tempfile.TemporaryDirectory(prefix='wyzar-rainbow-') as temporary:
        work=Path(temporary); code=work/'layout.cpp'; exe=work/'layout'
        code.write_text('#include <cstdio>\n#include <cstdint>\n#include <cstddef>\n#include "limine.h"\n'
            '#define WIN_EVENT_QUEUE_SIZE 64\n'+event+window+
            '\nint main(){printf("%zu %zu %zu %zu %zu",sizeof(Window),offsetof(Window,x),offsetof(Window,y),offsetof(Window,w),offsetof(Window,h));}\n')
        subprocess.run(['clang++','-std=c++20','-I',str(ROOT),str(code),'-o',str(exe)],check=True)
        size,xoff,yoff,woff,hoff=map(int,subprocess.check_output([str(exe)],text=True).split())
    smoke.IMAGE=args.image.resolve(); smoke.BOOT_ISO=False
    q=smoke.Qemu(extra_args=('-snapshot','-no-shutdown'),resolution=(1280,800))
    try:
        def memory(pointer,count):
            out=smoke.OUT/'rainbow-state.bin'
            assert not q.monitor(f'memsave 0x{pointer:x} {count} "{out}"')
            return out.read_bytes()
        def value(name,count=8): return int.from_bytes(memory(address(name),count),'little')
        def window(name):
            pointer=value(name); data=memory(pointer,size)
            return tuple(struct.unpack_from('<i',data,offset)[0] for offset in (xoff,yoff,woff,hoff))
        def move(x,y):
            for _ in range(80):
                mx=value('g_mouse_x',4); my=value('g_mouse_y',4)
                if (mx,my)==(x,y): return
                q.monitor(f'mouse_move {max(-100,min(100,x-mx))} {max(-100,min(100,y-my))}')
                time.sleep(.06)
            raise AssertionError('Pointer did not reach target')
        def drag(sx,sy,tx,ty):
            move(sx,sy); q.monitor('mouse_button 1'); time.sleep(.2)
            move(tx,ty); q.monitor('mouse_button 0'); time.sleep(.5)
        def measure(label):
            time.sleep(2)
            cpu=[]; frames=[]; client=[]; background=[]; windows=[]; scroll=[]
            for _ in range(24):
                assert q.command('query-status')['running']
                cpu.append(struct.unpack_from('<I',memory(usage,24),16)[0])
                frames.append(value('g_last_frame_ticks')); client.append(value('g_snap_rain'))
                background.append(value('g_snap_bg')); windows.append(value('g_snap_win'))
                scroll.append(value('rain_scroll',4)); time.sleep(.1)
            hz=value('tsc_hz')
            median=lambda values: sorted(values)[len(values)//2]
            result={'cpu_pct':median(cpu),'frame_ms':round(median(frames)*1000/hz,3),
                    'rain_ms':round(median(client)*1000/hz,3),'background_ms':round(median(background)*1000/hz,3),
                    'windows_ms':round(median(windows)*1000/hz,3),'scroll_advance':scroll[-1]-scroll[0]}
            if not args.baseline:
                result['renders']=value('g_rain_render_count')
                result['damaged_pixels']=value('g_rain_damage_pixels')
            print(label,result,flush=True)
            q.capture('rainbow-'+label+('-baseline' if args.baseline else ''))
            return result
        for _ in range(150):
            time.sleep(.5)
            try: log=q.capture('rainbow-boot')
            except AssertionError: continue
            if '[wm] Shell task started inside WM frame.' in log: break
        else: raise AssertionError(log[-4000:])
        partial=measure('partially-covered')
        rx,ry,rw,rh=window('g_win_rain'); tx,ty,tw,th=window('g_win_terminal')
        title=value('g_title_h',4)
        assert (rw,rh)==(tw,th)
        drag(tx+30,ty-title//2,rx+30,ry-title//2)
        assert window('g_win_terminal')[:2]==(rx,ry)
        if not args.baseline: renders=value('g_rain_render_count')
        covered=measure('fully-covered')
        if not args.baseline:
            assert covered['renders']==renders,'Covered rainbow kept rendering'
            assert covered['rain_ms']==0,'Hidden animation retained stale render cost'
        # Move the covering terminal away, then click the exposed rainbow chrome.
        drag(rx+30,ry-title//2,30,title//2)
        move(rx+rw-45,ry-title//2); q.monitor('mouse_button 1'); time.sleep(.15); q.monitor('mouse_button 0')
        if not args.baseline:
            before=value('g_rain_render_count'); time.sleep(.2)
            assert value('g_rain_render_count')>before,'Exposed animation did not resume'
        exposed=measure('exposed')
        assert exposed['scroll_advance']>0
        print('Rainbow visibility/drag regression passed.',flush=True)
    finally: q.close()


if __name__=='__main__': main()
