#!/usr/bin/env python3
"""WM GPU-hook integration using the test CPU backend, never a GPU benchmark.

Build a disposable ISO with CXX='clang++ -DGPU_COMPOSITOR_TEST', then run this
script. Checks animated pixels at completed frame boundaries and injects a
partial-batch failure with a replacement master buffer through the GDB stub.
"""
from pathlib import Path
import struct
import argparse
import subprocess
import time
import qemu_storage_smoke as smoke
from qemu_debug_memory import DebugMemory

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--rcs-timeout',action='store_true')
    rcs_timeout=parser.parse_args().rcs_timeout
    symbols=subprocess.check_output(['nm','-n','-C',str(ROOT/'wyzar')],text=True).splitlines()
    def address(name):
        return next(int(line.split()[0],16) for line in symbols if line.split()[-1]==name)
    boundary=next(int(line.split()[0],16) for line in symbols if 'wm_collect_retired_surfaces()' in line)
    # Fail before boot if accidentally given a production build.
    address('g_gpu_test_calls')
    present_test=any(line.split()[-1]=='g_gpu_test_present_calls' for line in symbols)
    render_test=any(line.split()[-1]=='g_gpu_test_render_calls' for line in symbols)
    assert not rcs_timeout or render_test
    smoke.BOOT_ISO=True
    q=smoke.Qemu(storage=False,resolution=(1280,800),extra_args=('-no-shutdown','-gdb','tcp:127.0.0.1:45456'))
    debug=None
    try:
        for _ in range(160):
            time.sleep(.25)
            assert q.command('query-status')['running']
            try:log=q.capture('gpu-test-boot')
            except AssertionError:continue
            if '[wm] Shell task started inside WM frame.' in log:break
        else:raise AssertionError(log[-4000:])
        q.command('stop');debug=DebugMemory()
        def value(name,length=8):return int.from_bytes(debug.read(address(name),length),'little')
        def frame():
            assert debug.request(f'Z0,{boundary:x},1')=='OK'
            assert debug.request('c').startswith(('T05','S05'))
            assert debug.request(f'z0,{boundary:x},1')=='OK'
            assert debug.request('s').startswith(('T05','S05'))
        for _ in range(4):frame()
        assert value('g_gpu_test_calls')>0
        assert value('g_gpu_test_rainbow_calls')>0
        if present_test:
            calls=value('g_gpu_test_present_calls');frame()
            assert value('g_gpu_test_present_calls')==calls+1,'Scene damage presented more than once per frame'
        # Look up Limine's pitch with a host helper, avoiding hard-coded ABI offsets.
        helper=smoke.OUT/'gpu-fb-offset.cpp';binary=smoke.OUT/'gpu-fb-offset'
        wm=(ROOT/'src/gui/wm.h').read_text()
        event='struct WinEvent {'+wm.split('struct WinEvent {',1)[1].split('\n};',1)[0]+'\n};'
        published='struct PublishedPixels {'+wm.split('struct PublishedPixels {',1)[1].split('\n};',1)[0]+'\n};'
        window=('struct Window {'+wm.split('struct Window {',1)[1].split('\n};',1)[0]+'\n};').replace('pid_t','uint32_t')
        helper.write_text('#include <cstdio>\n#include <cstddef>\n#include <cstdint>\n#include "limine.h"\n'
                          '#include "src/gui/window_resize.h"\n#define WIN_EVENT_QUEUE_SIZE 64\n'+published+event+window+
                          'int main(){printf("%zu %zu %zu %zu %zu %zu %zu",offsetof(limine_framebuffer,pitch),offsetof(Window,x),offsetof(Window,y),offsetof(Window,w),offsetof(Window,title),offsetof(Window,visible),offsetof(Window,h));}\n')
        subprocess.run(['clang++','-std=c++20','-I.',str(helper),'-o',str(binary)],check=True)
        offset,x_offset,y_offset,w_offset,title_offset,visible_offset,h_offset=map(int,subprocess.check_output([str(binary)]).split())
        pitch=int.from_bytes(debug.read(value('fb')+offset,8),'little')
        x,y,expected=struct.unpack('<III',debug.read(address('g_gpu_test_sample'),12))
        master=value('g_master_backbuffer')
        if render_test:
            # Ordinary resized clients use the actual RCS scene hook again.
            assert value('g_gpu_test_render_calls')>0
            sx,sy=1084,228
            pixel=0xFF000000|(32*3<<16)|(24*5<<8)|((32+24)*2)
            actual=int.from_bytes(debug.read(master+sy*pitch+sx*4,4),'little')
            assert actual==pixel,(sx,sy,hex(actual),hex(pixel))
            # Isolate adjacent RCS/BCS client animation. Confirm a retained
            # update, both clients' pixels and CPU grips, and no repaint of
            # an undamaged pixel elsewhere on the same scanline.
            visibility=[];fixtures=[]
            for index in range(value('g_window_count',4)):
                pointer=int.from_bytes(debug.read(address('g_window_list')+index*8,8),'little')
                name=debug.read(pointer+title_offset,64).split(b'\0')[0]
                if name not in (b'GPU scaled fixture',b'GPU native fixture'):
                    visibility.append((pointer,debug.read(pointer+visible_offset,1)))
                    debug.write(pointer+visible_offset,b'\0')
                else:fixtures.append(pointer)
            # Retire damage queued by boot and the windows hidden above before
            # injecting a sentinel outside this test's explicit damage region.
            for _ in range(4):frame()
            before=value('g_gpu_retained_fast_count')
            rcs_before=value('g_gpu_test_render_calls');bcs_before=value('g_gpu_test_calls')
            sentinel=0xFF2468AC
            debug.write(master+200*pitch+1000*4,sentinel.to_bytes(4,'little'))
            for name,number in [('g_dirty_min_x',1044),('g_dirty_min_y',180),
                                ('g_dirty_max_x',1275),('g_dirty_max_y',275)]:
                debug.write(address(name),number.to_bytes(4,'little'))
            debug.write(address('g_needs_refresh'),b'\1')
            frame()
            assert value('g_gpu_retained_fast_count')>before,'Mixed clients repainted the CPU scene'
            assert value('g_gpu_test_render_calls')>rcs_before and value('g_gpu_test_calls')>bcs_before
            for px,py,color in [(1084,228,pixel),(1200,200,0xFF113355),
                                (1146,270,0xFF81728F),(1274,270,0xFF81728F),(1000,200,sentinel)]:
                actual=int.from_bytes(debug.read(master+py*pitch+px*4,4),'little')
                assert actual==color,(px,py,hex(actual),hex(color))
            # Animation crossing the desktop HUD uses a small complete top
            # band and retained client pixels below it, with one presentation.
            for pointer in fixtures:debug.write(pointer+y_offset,(26).to_bytes(4,'little'))
            before=value('g_gpu_retained_fast_count')
            presentations=value('g_gpu_test_present_calls') if present_test else 0
            for name,number in [('g_dirty_min_x',1044),('g_dirty_min_y',26),
                                ('g_dirty_max_x',1275),('g_dirty_max_y',121)]:
                debug.write(address(name),number.to_bytes(4,'little'))
            debug.write(address('g_needs_refresh'),b'\1')
            frame()
            if present_test:assert value('g_gpu_test_present_calls')==presentations+1
            assert value('g_gpu_retained_fast_count')>before,'HUD crossing forced the whole client onto CPU'
            assert int.from_bytes(debug.read(master+49*pitch+1250*4,4),'little')==0xCC1A1F35,'GPU erased HUD'
            for pointer in fixtures:debug.write(pointer+y_offset,(180).to_bytes(4,'little'))
            for pointer,visible in visibility:debug.write(pointer+visible_offset,visible)
            for name,number in [('g_dirty_min_x',0),('g_dirty_min_y',0),
                                ('g_dirty_max_x',1279),('g_dirty_max_y',799)]:
                debug.write(address(name),number.to_bytes(4,'little'))
            debug.write(address('g_needs_refresh'),b'\1')
            for _ in range(4):frame()
        x,y,expected=struct.unpack('<III',debug.read(address('g_gpu_test_sample'),12))
        assert int.from_bytes(debug.read(master+y*pitch+x*4,4),'little')==expected
        assert not value('g_rain_surface_current',1),'GPU path kept rendering the full CPU rainbow'
        # Close a real surface-backed window through its control. Retirement
        # must invalidate cached GGTT descriptors before backing can be reused.
        debug.close();debug=None;q.command('cont')
        q.type('run /bin/win_test.elf\n',pause=.6)
        q.command('stop');debug=DebugMemory()
        for _ in range(3):frame()
        epoch=value('g_gpu_surface_epoch');windows=value('g_window_count',4)
        window_pointer=0
        for index in range(windows):
            pointer=int.from_bytes(debug.read(address('g_window_list')+index*8,8),'little')
            if debug.read(pointer+title_offset,64).split(b'\0')[0]==b'Wyzar Desktop Demo':
                window_pointer=pointer;break
        assert window_pointer,'Native GPU surface test window did not launch'
        title=value('g_title_h',4)
        wx=int.from_bytes(debug.read(window_pointer+x_offset,4),'little',signed=True)
        wy=int.from_bytes(debug.read(window_pointer+y_offset,4),'little',signed=True)
        ww=int.from_bytes(debug.read(window_pointer+w_offset,4),'little',signed=True)
        pad=max(2,title//10);button=min(title+8,(ww-2*pad-4)//3)
        def running_value(name):
            path=smoke.OUT/'gpu-pointer-state.bin'
            assert not q.monitor(f'memsave 0x{address(name):x} 4 "{path}"')
            return int.from_bytes(path.read_bytes(),'little',signed=True)
        if render_test:
            # Resize the real published 480x320 demo through the actual corner
            # before maximizing. Odd display dimensions must activate RCS.
            hh=int.from_bytes(debug.read(window_pointer+h_offset,4),'little',signed=True)
            demo_before=value('g_gpu_test_demo_render_calls')
            target_x=wx+ww-2;target_y=wy+hh-2
            debug.close();debug=None;q.command('cont')
            for _ in range(80):
                dx=target_x-running_value('g_mouse_x');dy=target_y-running_value('g_mouse_y')
                if not dx and not dy:break
                q.monitor(f'mouse_move {max(-80,min(80,dx))} {max(-80,min(80,dy))}');time.sleep(.04)
            else:raise AssertionError('Pointer did not reach demo resize corner')
            q.monitor('mouse_button 1');time.sleep(.15)
            target_x+=159;target_y+=157
            for _ in range(80):
                dx=target_x-running_value('g_mouse_x');dy=target_y-running_value('g_mouse_y')
                if not dx and not dy:break
                q.monitor(f'mouse_move {max(-80,min(80,dx))} {max(-80,min(80,dy))}');time.sleep(.04)
            else:raise AssertionError('Pointer did not complete demo resize drag')
            q.monitor('mouse_button 0');time.sleep(.5)
            q.command('stop');debug=DebugMemory()
            for _ in range(6):frame()
            ww=int.from_bytes(debug.read(window_pointer+w_offset,4),'little',signed=True)
            hh=int.from_bytes(debug.read(window_pointer+h_offset,4),'little',signed=True)
            assert (ww,hh)==(639,477),('Demo resize dimensions',ww,hh)
            assert value('g_gpu_test_demo_render_calls')>demo_before,'Resized demo did not use RCS'
            target_x=wx+ww-pad-2*button-2+button//2;target_y=wy-title//2
            fast_before=value('g_gpu_scaled_fast_count')
            debug.close();debug=None;q.command('cont')
            for _ in range(80):
                dx=target_x-running_value('g_mouse_x');dy=target_y-running_value('g_mouse_y')
                if not dx and not dy:break
                q.monitor(f'mouse_move {max(-80,min(80,dx))} {max(-80,min(80,dy))}');time.sleep(.04)
            else:raise AssertionError('Pointer did not reach demo maximize')
            q.monitor('mouse_button 1');time.sleep(.15);q.monitor('mouse_button 0');time.sleep(.4)
            q.command('stop');debug=DebugMemory()
            for _ in range(6):frame()
            assert value('g_gpu_scaled_fast_count')>fast_before,'Scaled animation repainted the covered desktop'
            sx,sy,pixel=struct.unpack('<III',debug.read(address('g_gpu_test_scaled_sample'),12))
            assert int.from_bytes(debug.read(master+sy*pitch+sx*4,4),'little')==pixel
            calls=value('g_gpu_test_render_calls')
            debug.write(address('g_gpu_test_render_fail'),bytes([2 if rcs_timeout else 1]))
            for name,number in [('g_dirty_min_x',0),('g_dirty_min_y',0),
                                ('g_dirty_max_x',1279),('g_dirty_max_y',799)]:
                debug.write(address(name),number.to_bytes(4,'little'))
            debug.write(address('g_needs_refresh'),b'\1')
            for _ in range(4):frame()
            assert value('g_gpu_test_render_calls')==calls+1
            assert value('g_gpu_render_hook')==0
            if rcs_timeout:
                assert value('g_gpu_compose_hook')==value('g_gpu_present_hook')==value('g_gpu_flip_hook')==0
                assert value('g_master_backbuffer')!=master and value('g_gpu_quarantine_sources',1)
                assert b'CPU fallback PREDRAW' in debug.read(address('g_gpu_status'),64)
                debug.close();debug=None;q.command('cont')
                before=running_value('g_mouse_x')
                q.monitor('mouse_move 4 0');time.sleep(.3)
                assert running_value('g_mouse_x')>before
                print('RCS desktop timeout: replacement buffer, CPU frames and pointer input passed.',flush=True)
                return
            assert value('g_gpu_compose_hook')!=0 and value('g_master_backbuffer')==master
            wx=int.from_bytes(debug.read(window_pointer+x_offset,4),'little',signed=True)
            wy=int.from_bytes(debug.read(window_pointer+y_offset,4),'little',signed=True)
            ww=int.from_bytes(debug.read(window_pointer+w_offset,4),'little',signed=True)
            button=min(title+8,(ww-2*pad-4)//3)
        target_x=wx+ww-pad-button//2;target_y=wy-title//2
        debug.close();debug=None;q.command('cont')
        for _ in range(80):
            dx=target_x-running_value('g_mouse_x');dy=target_y-running_value('g_mouse_y')
            if not dx and not dy:break
            q.monitor(f'mouse_move {max(-80,min(80,dx))} {max(-80,min(80,dy))}');time.sleep(.04)
        else:raise AssertionError('Pointer did not reach the window close control')
        q.monitor('mouse_button 1');time.sleep(.15);q.monitor('mouse_button 0');time.sleep(.4)
        q.command('stop');debug=DebugMemory()
        for _ in range(3):frame()
        assert value('g_window_count',4)==windows-1,'Real window close failed'
        assert value('g_gpu_surface_epoch')>epoch,'Retired surface did not invalidate GGTT bindings'
        assert value('g_gpu_compose_hook')!=0
        # Maximize Rainbow through its actual control and inspect a completed
        # retained-scene frame. The GPU path must bypass covered scene repaint.
        pointer=value('g_win_rain')
        wx=int.from_bytes(debug.read(pointer+x_offset,4),'little',signed=True)
        wy=int.from_bytes(debug.read(pointer+y_offset,4),'little',signed=True)
        ww=int.from_bytes(debug.read(pointer+w_offset,4),'little',signed=True)
        button=min(title+8,(ww-2*pad-4)//3)
        target_x=wx+ww-pad-2*button-2+button//2;target_y=wy-title//2
        fast_before=value('g_gpu_fullscreen_fast_count')
        debug.close();debug=None;q.command('cont')
        for _ in range(80):
            dx=target_x-running_value('g_mouse_x');dy=target_y-running_value('g_mouse_y')
            if not dx and not dy:break
            q.monitor(f'mouse_move {max(-80,min(80,dx))} {max(-80,min(80,dy))}');time.sleep(.04)
        else:raise AssertionError('Pointer did not reach Rainbow maximize')
        q.monitor('mouse_button 1');time.sleep(.15);q.monitor('mouse_button 0');time.sleep(.4)
        q.command('stop');debug=DebugMemory()
        for _ in range(3):frame()
        assert value('g_gpu_fullscreen_fast_count')>fast_before,'Fullscreen Rainbow repainted the covered scene'
        x,y,expected=struct.unpack('<III',debug.read(address('g_gpu_test_sample'),12))
        assert int.from_bytes(debug.read(master+y*pitch+x*4,4),'little')==expected
        if present_test:
            calls=value('g_gpu_test_present_calls')
            debug.write(address('g_gpu_test_present_fail'),b'\1')
            for _ in range(4):frame()
            assert value('g_gpu_test_present_calls')==calls+1
            assert value('g_gpu_present_hook')==0,'Failed presenter remained installed'
            assert value('g_gpu_compose_hook')!=0,'Display failure unnecessarily disabled composition'
            assert value('g_master_backbuffer')==master
        before=value('g_gpu_test_calls')
        debug.write(address('g_gpu_test_fail'),b'\1')
        for _ in range(5):frame()
        assert value('g_gpu_test_calls')==before+1
        assert value('g_gpu_compose_hook')==0
        assert value('g_master_backbuffer')!=master,'Quarantine did not replace the master buffer'
        assert value('g_gpu_quarantine_sources',1),'Late-DMA sources were not pinned'
        assert value('g_rain_surface_current',1),'CPU rainbow did not resume after fallback'
        # A second frame boundary proves the fallback keeps running and the
        # retired-surface collector is reached again after the failed batch.
        frame();assert value('g_gpu_test_calls')==before+1
        debug.close();debug=None;q.command('cont')
        mouse_before=running_value('g_mouse_x')
        q.monitor('mouse_move 4 0');time.sleep(.3)
        assert running_value('g_mouse_x')>mouse_before,'Pointer input stalled after fallback'
        log=q.capture('gpu-test-fallback')
        assert 'KERNEL PANIC' not in log and q.command('query-status')['running']
        print('WM GPU scaled/fullscreen pixels, retained scene, frame presentation, surface retirement and CPU fallback passed.',flush=True)
    except Exception:
        q.command('stop')
        q.capture('gpu-test-failure')
        print(q.monitor('info registers'),flush=True)
        raise
    finally:
        if debug:debug.close()
        q.close()

if __name__=='__main__':main()
