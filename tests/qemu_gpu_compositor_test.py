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
    parser.add_argument('--large-alpha',action='store_true')
    args=parser.parse_args()
    rcs_timeout=args.rcs_timeout
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
    q=smoke.Qemu(storage=False,resolution=(1920,1080) if args.large_alpha else (1280,800),extra_args=('-no-shutdown','-gdb','tcp:127.0.0.1:45456'))
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
        # Exercise RCS and its failure paths explicitly. A separate comparison
        # below enables the production CPU alpha threshold for the same pixels.
        if not args.large_alpha:debug.write(address('g_cpu_alpha_pixel_limit'),bytes(8))
        def frame():
            assert debug.request(f'Z0,{boundary:x},1')=='OK'
            assert debug.request('c').startswith(('T05','S05'))
            assert debug.request(f'z0,{boundary:x},1')=='OK'
            # Run through the collector rather than single-step its entry.
            # A pending IRQ can consume that step before the entry instruction,
            # causing the next call to stop on the very same frame boundary.
            registers=bytes.fromhex(debug.request('g'))
            assert len(registers)>=64
            stack=int.from_bytes(registers[56:64],'little') # x86-64 RSP
            returned=int.from_bytes(debug.read(stack,8),'little')
            assert debug.request(f'Z0,{returned:x},1')=='OK'
            assert debug.request('c').startswith(('T05','S05'))
            assert debug.request(f'z0,{returned:x},1')=='OK'
        for _ in range(4):frame()
        debug.write(address('g_gpu_test_scene_repair_request'),b'\4')
        frame()
        assert value('g_gpu_test_scene_repair_accepted',1),'Opaque taskbar popup rejected retained animation or its cutout'
        print('Retained app picker: cutout and full panel pixels survive corruption, stale clips and hover on both destination indices.',flush=True)
        debug.write(address('g_gpu_test_scene_repair_request'),b'\5')
        frame()
        assert value('g_gpu_test_scene_repair_accepted',1),'Volume popup lost its retained cutout or content repaint'
        print('Retained volume popup: full pixels, slider/mute changes, stale clips and both destination indices passed.',flush=True)
        debug.write(address('g_gpu_test_scene_repair_request'),b'\7')
        frame()
        assert value('g_gpu_test_scene_repair_accepted',1),'Overflow popup cache, exact clips or retained replay failed'
        print('Shared popup cache: app groups, volume and overflow reuse pixels across animation frames; cached and allocation-failure repairs preserve exact clips.',flush=True)
        debug.write(address('g_gpu_test_scene_repair_request'),b'\6')
        frame()
        assert value('g_gpu_test_scene_repair_accepted',1),'Covered-log volume updates repainted the log or escaped cheap retained repairs'
        print('Volume under opaque fullscreen Rainbow: covered Boot Log unchanged, tray/panel repairs submit no GPU work, complete popup pixels preserved.',flush=True)
        for request in (8,9):
            debug.write(address('g_gpu_test_scene_repair_request'),bytes([request]))
            frame()
            assert value('g_gpu_test_scene_repair_accepted',1),'Clipboard blocked retained Rainbow, rebuilt static content, lost exact clips or failed selection/close'
        print('Clipboard over fullscreen Rainbow: populated/empty panels, hover, pin, scroll, delete, paste/close, exact clips and allocation fallback passed; animation submissions continue without rebuilding static popup pixels.',flush=True)
        assert value('g_gpu_test_calls')>0
        assert value('g_gpu_test_rainbow_calls')>0
        assert value('g_titlebar_cache_renders')>0
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
                          'int main(){printf("%zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu",offsetof(limine_framebuffer,pitch),offsetof(Window,x),offsetof(Window,y),offsetof(Window,w),offsetof(Window,title),offsetof(Window,visible),offsetof(Window,h),offsetof(Window,premultiplied),offsetof(Window,buffer),offsetof(Window,shadow_enabled),offsetof(Window,maximized),offsetof(Window,published),offsetof(PublishedPixels,pixels),offsetof(PublishedPixels,width),offsetof(PublishedPixels,revision),offsetof(Window,canvas_width),offsetof(Window,canvas_height),offsetof(Window,buffer_pitch));}\n')
        subprocess.run(['clang++','-std=c++20','-I.',str(helper),'-o',str(binary)],check=True)
        offset,x_offset,y_offset,w_offset,title_offset,visible_offset,h_offset,alpha_offset,buffer_offset,shadow_offset,maximized_offset,published_offset,pixels_offset,dimensions_offset,revision_offset,canvas_w,canvas_h,canvas_pitch=map(int,subprocess.check_output([str(binary)]).split())
        task_source=(ROOT/'src/core/task.h').read_text()
        task=('struct Task {'+task_source.split('struct Task {',1)[1].split('\n};',1)[0]+'\n};').replace('pid_t','uint32_t')
        helper.write_text('#include <cstdio>\n#include <cstddef>\n#include <cstdint>\n'
                          '#define MAX_FDS 16\nstruct file;enum class TaskState:uint8_t;\n'+task+
                          'int main(){printf("%zu %zu %zu",offsetof(Task,next),offsetof(Task,stack_base),offsetof(Task,stack_size));}\n')
        subprocess.run(['clang++','-std=c++20','-I.',str(helper),'-o',str(binary)],check=True)
        task_next,task_stack,task_size=map(int,subprocess.check_output([str(binary)]).split())
        def check_memory():
            head=value('g_task_list');pointer=head;compositor_free=0
            for _ in range(128):
                base=int.from_bytes(debug.read(pointer+task_stack,8),'little')
                size=int.from_bytes(debug.read(pointer+task_size,8),'little')
                pid=int.from_bytes(debug.read(pointer,4),'little')
                if base and size:
                    assert int.from_bytes(debug.read(base,8),'little')==0x57595a415253544b,(pid,'stack canary')
                    if pid==1:
                        stack=b''.join(debug.read(base+i,min(1024,size-i)) for i in range(0,size,1024))
                        compositor_free=next((i for i,b in enumerate(stack[8:],8) if b),size)
                        assert compositor_free>=16384,('Compositor stack headroom',compositor_free)
                pointer=int.from_bytes(debug.read(pointer+task_next,8),'little')
                if pointer==head:break
            else:raise AssertionError('Task list does not close')
            previous=0;pointer=value('g_heap_head');blocks=0
            while pointer:
                size,free,prior,following,magic,reserved=struct.unpack('<QB7xQQQQ',debug.read(pointer,48))
                assert magic==0xDEADBEEFCAFEBABE and prior==previous and size>=48 and size%16==0,('Heap header',hex(pointer))
                previous,pointer=pointer,following;blocks+=1
                assert blocks<100000,'Heap list cycle'
            assert previous==value('g_heap_tail')
            print(f'Heap headers intact ({blocks} blocks); compositor unused stack prefix {compositor_free} bytes.',flush=True)
        pitch=int.from_bytes(debug.read(value('fb')+offset,8),'little')
        x,y,expected=struct.unpack('<III',debug.read(address('g_gpu_test_sample'),12))
        master=value('g_master_backbuffer')
        if args.large_alpha:
            assert render_test and value('g_cpu_alpha_pixel_limit')==2**64-1
            count=value('g_window_count',4)
            windows=struct.unpack('<'+'Q'*count,debug.read(address('g_window_list'),count*8))
            rain=value('g_win_rain')
            upper=next(w for w in windows if debug.read(w+title_offset,64).split(b'\0')[0]==b'GPU native fixture')
            for w in windows:debug.write(w+visible_offset,bytes([w in (rain,upper)]))
            ordering=[i for i,w in enumerate(windows) if w not in (rain,upper)]+[windows.index(rain),windows.index(upper)]
            debug.write(address('g_window_order'),struct.pack('<'+'i'*count,*ordering))
            title=value('g_title_h',4);height=1080-title-value('g_taskbar_h',4)
            debug.write(rain+x_offset,struct.pack('<iiii',0,title,1920,height))
            for off,n in [(canvas_w,1920),(canvas_h,height),(canvas_pitch,1920*4)]:
                debug.write(rain+off,n.to_bytes(4,'little'))
            debug.write(rain+maximized_offset,b'\1');debug.write(rain+shadow_offset,b'\0')
            debug.write(upper+alpha_offset,b'\1');debug.write(upper+shadow_offset,b'\1')
            buffer=int.from_bytes(debug.read(upper+buffer_offset,8),'little')
            row=b''.join(struct.pack('<I',0 if x<43 else 0x80402010 if x<86 else 0xff235789) for x in range(128))
            pixels=row*96
            for i in range(0,len(pixels),1024):debug.write(buffer+i,pixels[i:i+1024])
            def over(src,dst):
                alpha=src>>24
                if not alpha:return dst
                return 0xff000000|sum(min(255,((src>>s)&255)+(((dst>>s)&255)*(255-alpha)+127)//255)<<s for s in (0,8,16))
            for width,height in [(900,650),(1300,900),(1800,950)]:
                debug.write(upper+x_offset,struct.pack('<iiii',40,80,width,height))
                for name,n in [('g_dirty_min_x',0),('g_dirty_min_y',0),('g_dirty_max_x',1919),('g_dirty_max_y',1079)]:
                    debug.write(address(name),n.to_bytes(4,'little'))
                debug.write(address('g_needs_refresh'),b'\1')
                rcs=value('g_gpu_test_render_calls');bcs=value('g_gpu_test_rainbow_calls')
                for _ in range(4):
                    frame()
                    assert value('g_gpu_test_render_calls')==rcs,('Size switched alpha to RCS',width,height)
                    scroll=value('rain_scroll',4)
                    for px,source in [(80,0),(40+width//2,0x80402010),(40+width-20,0xff235789)]:
                        background=int.from_bytes(debug.read(address('g_rainbow_wide')+((scroll+px)%720)*4,4),'little')
                        actual=int.from_bytes(debug.read(master+400*pitch+px*4,4),'little')
                        assert actual==over(source,background),(width,height,px,hex(actual),hex(over(source,background)))
                assert value('g_gpu_test_rainbow_calls')>bcs
            check_memory()
            print('Alpha resize 900x650 -> 1300x900 -> 1800x950: exact animated Rainbow pixels, no RCS transition, intact heap/stacks passed.',flush=True)
            return
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
            # Alpha clients must blend in stack order with freshly reconstructed
            # background, including tiny damage and CPU grip rows. Compare
            # ordered RCS to CPU for two overlapping premultiplied surfaces.
            lower,upper=fixtures
            upper_x=debug.read(upper+x_offset,4)
            upper_y=debug.read(upper+y_offset,4)
            wallpaper=debug.read(address('g_wallpaper'),8)
            render_hook=debug.read(address('g_gpu_render_hook'),8)
            def read_surface(buffer,size):
                return b''.join(debug.read(buffer+n,min(1024,size-n)) for n in range(0,size,1024))
            def write_surface(buffer,data):
                for n in range(0,len(data),1024):debug.write(buffer+n,data[n:n+1024])
            backing=[]
            for pointer,size in [(lower,64*48*4),(upper,128*96*4)]:
                buffer=int.from_bytes(debug.read(pointer+buffer_offset,8),'little')
                backing.append((buffer,read_surface(buffer,size)))
            debug.write(address('g_wallpaper'),bytes(8))
            debug.write(upper+x_offset,(1040).to_bytes(4,'little'))
            debug.write(upper+y_offset,(200).to_bytes(4,'little'))
            def repaint(x1=0,y1=150,x2=1279,y2=310):
                for name,number in [('g_dirty_min_x',x1),('g_dirty_min_y',y1),
                                    ('g_dirty_max_x',x2),('g_dirty_max_y',y2)]:
                    debug.write(address(name),number.to_bytes(4,'little'))
                debug.write(address('g_needs_refresh'),b'\1');frame()
            def over(s,d):
                a=s>>24
                return 0xff000000|sum(min(255,((s>>b)&255)+(((d>>b)&255)*(255-a)+127)//255)<<b for b in (0,8,16))
            def pixel_at(px,py):return int.from_bytes(debug.read(master+py*pitch+px*4,4),'little')
            debug.write(upper+alpha_offset,b'\1')
            write_surface(backing[1][0],struct.pack('<I',0x80402010)*(128*96))
            calls=value('g_gpu_test_render_calls');repaint()
            assert value('g_gpu_test_render_calls')>calls
            assert pixel_at(1084,228)==over(0x80402010,pixel),'Alpha erased the underlying scaled client'
            assert pixel_at(1152,290)==over(0x80402010,0xff008080),'CPU grip rows failed alpha blending'
            # Repeated one-pixel underlying damage must not touch an unrelated
            # pixel on that row or accumulate alpha in the updated pixel.
            sentinel=0xff7351a9
            debug.write(master+228*pitch+1152*4,sentinel.to_bytes(4,'little'))
            for _ in range(3):
                rendered=value('g_gpu_test_rcs_pixels')
                repaint(1084,228,1084,228)
                assert value('g_gpu_test_rcs_pixels')-rendered==2,'One-pixel alpha damage submitted oversized RCS rectangles'
                assert pixel_at(1084,228)==over(0x80402010,pixel)
                assert pixel_at(1152,228)==sentinel,'Ordered alpha repainted outside damage'
            debug.write(lower+alpha_offset,b'\1')
            write_surface(backing[0][0],struct.pack('<I',0x80601020)*(64*48))
            repaint()
            assert pixel_at(1084,228)==over(0x80402010,over(0x80601020,0xff008080))
            samples=[(1084,228),(1152,228),(1152,290),(1030,210),(1050,174)]
            accelerated=[pixel_at(px,py) for px,py in samples]
            # Exercise the real shared compositor on disjoint retained repairs.
            # Overlapping input is canonicalized; each alpha layer submits once,
            # and the undamaged gap between clips must keep its sentinel.
            repair_clips=[(1080,220,1088,226),(1084,224,1092,230),(1100,220,1108,230),
                          (0,0,1280,1),(0,799,1280,800)]
            def batch_repairs():
                region=address('g_gpu_test_scene_repairs')
                debug.write(region,b''.join(struct.pack('<iiii',*r) for r in repair_clips))
                debug.write(region+64*16,len(repair_clips).to_bytes(4,'little'))
                debug.write(address('g_gpu_test_scene_repair_request'),b'\1')
                frame()
                assert value('g_gpu_test_scene_repair_accepted',1),'Shared repair batch rejected interior clips'
            sentinel=0xff7351a9
            debug.write(master+225*pitch+1095*4,sentinel.to_bytes(4,'little'))
            for _ in range(3):
                draws=value('g_gpu_test_render_calls')
                batch_repairs()
                assert value('g_gpu_test_render_calls')-draws==2,'Repair clips submitted RCS more than once per layer'
                assert pixel_at(1095,225)==sentinel,'Batched repairs filled an undamaged hole'
                for px,py in [(1084,225),(1090,228),(1104,225)]:
                    assert pixel_at(px,py)==over(0x80402010,over(0x80601020,0xff008080)),('Batched alpha accumulated',px,py)
            debug.write(address('g_gpu_test_render_fail'),b'\1')
            batch_repairs()
            assert value('g_gpu_render_hook')==0
            assert [pixel_at(px,py) for px,py in samples]==accelerated,'Partial repair batch fallback double-blended alpha'
            debug.write(address('g_gpu_test_render_fail'),b'\0')
            debug.write(address('g_gpu_render_hook'),render_hook)
            repaint()
            print('Shared scene repairs: one RCS submission per layer, exact holes, overlapping clips and partial-failure CPU recovery passed.',flush=True)
            if value('g_gpu_test_tray_enabled',1):
                # Clock/FPS updates must match full taskbar rendering while touching
                # only their exact strip, including borders and antialiased text.
                tray_left=value('g_gpu_test_tray_left',4)
                tray_top=800-value('g_taskbar_h',4)
                old_fps=value('g_desktop_fps',4)
                def tray_repair(fps,clips=None):
                    debug.write(address('g_gpu_test_tray_fps'),fps.to_bytes(4,'little'))
                    if clips is not None:
                        region=address('g_gpu_test_scene_repairs')
                        debug.write(region,b''.join(struct.pack('<iiii',*r) for r in clips))
                        debug.write(region+64*16,len(clips).to_bytes(4,'little'))
                    debug.write(address('g_gpu_test_scene_repair_request'),bytes([2 if clips is not None else 3]))
                    frame()
                    if clips is not None:assert value('g_gpu_test_scene_repair_accepted',1)
                def tray_pixels(snapshot=False):
                    base=value('g_gpu_test_taskbar_before') if snapshot else master+tray_top*pitch
                    return b''.join(debug.read(base+y*pitch+tray_left*4,(1280-tray_left)*4) for y in range(800-tray_top))
                tray_clips=[(tray_left,tray_top,1280,800),
                            (tray_left+3,tray_top+4,1277,798)]
                for fps in (8,123,60):
                    debug.write(address('g_gpu_test_tray_menu'),bytes([fps==123]))
                    tray_repair(fps);expected=tray_pixels()
                    for clip in tray_clips:
                        tray_repair(987)
                        bcs=value('g_gpu_test_calls');rcs=value('g_gpu_test_render_calls')
                        tray_repair(fps,[clip]);actual=tray_pixels();before=tray_pixels(True)
                        assert value('g_gpu_test_calls')==bcs and value('g_gpu_test_render_calls')==rcs
                        preserved=int.from_bytes(debug.read(value('g_gpu_test_taskbar_before')+10*pitch+20*4,4),'little')
                        assert pixel_at(20,tray_top+10)==preserved,'Tray repair repainted a task button'
                        for y in range(tray_top,800):
                            for x in range(tray_left,1280):
                                p=((y-tray_top)*(1280-tray_left)+x-tray_left)*4
                                target=expected if clip[0]<=x<clip[2] and clip[1]<=y<clip[3] else before
                                assert actual[p:p+4]==target[p:p+4],('Tray clip mismatch',fps,x,y)
                debug.write(address('g_gpu_test_tray_menu'),b'\0')
                tray_repair(old_fps)
                print('Retained FPS tray: full/partial clips match full taskbar pixels, preserve task buttons and submit no GPU work.',flush=True)
            cpu_before=value('g_cpu_alpha_pixel_limit')
            debug.write(address('g_cpu_alpha_pixel_limit'),(1024*1024).to_bytes(8,'little'))
            rcs_before=value('g_gpu_test_render_calls');repaint()
            assert value('g_gpu_test_render_calls')==rcs_before,'Bounded alpha still submitted RCS'
            assert [pixel_at(px,py) for px,py in samples]==accelerated,'SIMD alpha differs from RCS'
            debug.write(address('g_cpu_alpha_pixel_limit'),cpu_before.to_bytes(8,'little'))
            debug.write(address('g_gpu_render_hook'),bytes(8));repaint()
            assert [pixel_at(px,py) for px,py in samples]==accelerated,'RCS/CPU alpha outputs differ'
            # Inject failure after a partial blend. The retry must reconstruct
            # the complete scene and disable RCS, preserving the CPU result.
            debug.write(address('g_gpu_render_hook'),render_hook)
            debug.write(address('g_gpu_test_render_fail'),b'\1');repaint()
            assert value('g_gpu_render_hook')==0
            assert [pixel_at(px,py) for px,py in samples]==accelerated,'Alpha fallback double-blended pixels'
            debug.write(address('g_gpu_test_render_fail'),b'\0')
            debug.write(address('g_gpu_render_hook'),render_hook)
            # One immutable foreground snapshot is staged once across repeated
            # background repaint, then replaced when the slot publishes again.
            debug.write(lower+visible_offset,b'\0')
            slot=upper+published_offset
            debug.write(slot+pixels_offset,backing[1][0].to_bytes(8,'little'))
            debug.write(slot+dimensions_offset,struct.pack('<ii',128,96))
            debug.write(slot+revision_offset,(1000001).to_bytes(8,'little'))
            repaint()
            uploaded=value('g_gpu_test_uploads');reused=value('g_gpu_test_upload_reuses')
            for _ in range(4):repaint(1084,228,1084,228)
            assert value('g_gpu_test_uploads')==uploaded,'Unchanged immutable foreground was uploaded again'
            assert value('g_gpu_test_upload_reuses')>reused
            color=0x80201008
            debug.write(backing[1][0]+(28*128+44)*4,color.to_bytes(4,'little'))
            debug.write(slot+revision_offset,(1000002).to_bytes(8,'little'))
            repaint(1084,228,1084,228)
            assert value('g_gpu_test_uploads')==uploaded+1,'New publication reused stale staged pixels'
            assert pixel_at(1084,228)==over(color,0xff008080)
            debug.write(slot+pixels_offset,bytes(8));debug.write(slot+revision_offset,bytes(8))
            debug.write(lower+visible_offset,b'\1')
            for pointer in fixtures:debug.write(pointer+alpha_offset,b'\0')
            for buffer,data in backing:write_surface(buffer,data)
            # Rainbow stays on the resident-palette BCS path beneath alpha.
            # With an opaque covering window its animation is fully hidden;
            # enabling alpha exposes it without CPU full-surface generation.
            rain=value('g_win_rain')
            rain_geometry=debug.read(rain+x_offset,16)
            upper_geometry=debug.read(upper+x_offset,16)
            rx,ry,rw,rh=struct.unpack('<iiii',rain_geometry)
            debug.write(lower+visible_offset,b'\0');debug.write(rain+visible_offset,b'\1')
            debug.write(rain+x_offset,struct.pack('<iiii',100,160,rw,rh))
            debug.write(upper+x_offset,struct.pack('<iiii',100,160,rw,rh))
            # A smaller opaque window creates several exposed Rainbow strips.
            # They must share one scene submission, not one wait per strip.
            debug.write(upper+x_offset,struct.pack('<iiii',100+rw//4,160+rh//4,rw//2,rh//2))
            repaint(0,0,1279,799)
            for _ in range(2):frame()
            blits=value('g_gpu_test_calls');palettes=value('g_gpu_test_rainbow_calls')
            for _ in range(4):frame()
            assert value('g_gpu_test_calls')-blits<=5,'Opaque overlap submitted a blit batch per exposed strip'
            assert value('g_gpu_test_rainbow_calls')>palettes
            debug.write(upper+x_offset,struct.pack('<iiii',100,160,rw,rh))
            repaint(0,0,1279,799)
            for _ in range(3):frame()
            animations=value('g_rain_render_count');bcs_calls=value('g_gpu_test_rainbow_calls')
            debug.write(upper+alpha_offset,b'\1');repaint(0,0,1279,799)
            for _ in range(4):frame()
            assert value('g_rain_render_count')>animations,'Alpha hid underlying Rainbow animation'
            assert value('g_gpu_test_rainbow_calls')>bcs_calls,'Alpha moved Rainbow off the GPU palette path'
            assert not value('g_rain_surface_current',1),'Alpha regenerated the full CPU Rainbow surface'
            # A transparent layer crossing the HUD must blend once per frame,
            # rather than repeat RCS for a top band and the lower client band.
            debug.write(rain+y_offset,(26).to_bytes(4,'little'))
            debug.write(upper+y_offset,(26).to_bytes(4,'little'))
            repaint(0,0,1279,799)
            draws=value('g_gpu_test_render_calls');blits=value('g_gpu_test_calls')
            presents=value('g_gpu_test_present_calls') if present_test else 0
            for _ in range(4):frame()
            # Allow one independent periodic HUD refresh during the interval.
            assert 4<=value('g_gpu_test_render_calls')-draws<=5,'HUD crossing repeated the alpha submission'
            assert value('g_gpu_test_calls')-blits<=6,'HUD crossing fragmented Rainbow into extra blit batches'
            if present_test:assert value('g_gpu_test_present_calls')-presents==4
            assert pixel_at(600,49)==0xCC1A1F35,'Alpha/Rainbow overwrote the HUD'
            debug.write(upper+alpha_offset,b'\0');debug.write(rain+visible_offset,b'\0')
            debug.write(rain+x_offset,rain_geometry);debug.write(upper+x_offset,upper_geometry)
            debug.write(lower+visible_offset,b'\1')

            # Cached shadows must darken both GPU-composed client pixels and
            # wallpaper, remain stable under tiny damage, and disappear on
            # maximize/hide. An ordinary interior update still uses retention.
            repaint(0,0,1279,799) # retire the previous fixture geometry too
            title=value('g_title_h',4)
            shadow_samples=[(1036,228),(1170,228),(1152,300),(1100,230)]
            base=[pixel_at(px,py) for px,py in shadow_samples]
            def shadow_alpha(px,py):
                if 1040<=px<1168 and 200-title<=py<296:return 0
                dx=max(1040-px,px-1168+1,0)
                dy=max(200-title+4-py,py-(296+4)+1,0)
                remaining=max(0,144-dx*dx-dy*dy)
                return 64*remaining*remaining//(144*144)
            expected_shadow=[over(shadow_alpha(px,py)<<24,color) for (px,py),color in zip(shadow_samples,base)]
            debug.write(upper+shadow_offset,b'\1');repaint()
            assert [pixel_at(px,py) for px,py in shadow_samples]==expected_shadow,'Shadow stack order or pixels differ'
            q.capture('gpu-shadow-preview')
            for _ in range(3):
                repaint(1036,228,1036,228)
                assert pixel_at(1036,228)==expected_shadow[0],'Shadow opacity accumulated on partial repaint'
            retained=value('g_gpu_retained_fast_count')
            repaint(1080,220,1100,240)
            assert value('g_gpu_retained_fast_count')>retained,'Interior animation unnecessarily recomposed shadows'
            debug.write(address('g_gpu_render_hook'),bytes(8));repaint()
            assert [pixel_at(px,py) for px,py in shadow_samples]==expected_shadow,'CPU and GPU shadow scenes differ'
            debug.write(address('g_gpu_render_hook'),render_hook)
            debug.write(upper+maximized_offset,b'\1');repaint()
            assert pixel_at(1170,228)==base[1] and pixel_at(1152,300)==base[2],'Maximize left shadow pixels'
            debug.write(upper+maximized_offset,b'\0');repaint()
            assert pixel_at(1170,228)==expected_shadow[1]
            debug.write(upper+visible_offset,b'\0');repaint()
            assert pixel_at(1170,228)==base[1] and pixel_at(1152,300)==base[2],(
                'Hiding a window left a shadow',hex(pixel_at(1170,228)),hex(base[1]),hex(pixel_at(1152,300)),hex(base[2]))
            debug.write(upper+visible_offset,b'\1');debug.write(upper+shadow_offset,b'\0')
            # Warm title bitmaps, then repaint both headers without rerasterizing.
            repaint()
            builds=value('g_titlebar_cache_renders');hits=value('g_titlebar_cache_hits')
            for _ in range(3):repaint(1020,200-title,1167,199)
            assert value('g_titlebar_cache_renders')==builds,'Unchanged titles were rasterized again'
            assert value('g_titlebar_cache_hits')>hits
            saved_title=debug.read(upper+title_offset,64)
            debug.write(upper+title_offset,b'Changed title\0');repaint(1040,200-title,1167,199)
            assert value('g_titlebar_cache_renders')>builds,'Title mutation reused stale pixels'
            debug.write(upper+title_offset,saved_title)
            old_focus=debug.read(address('g_focused_win_id'),4)
            window_id=debug.read(upper,4)
            debug.write(address('g_focused_win_id'),window_id);repaint(1040,200-title,1167,199)
            assert pixel_at(1040,200-title)==0xff77608f,'Focused title did not update'
            epoch=value('g_decoration_epoch')
            builds=value('g_titlebar_cache_renders')
            debug.write(address('g_decoration_epoch'),(epoch+1).to_bytes(8,'little'))
            repaint(1040,200-title,1167,199)
            assert value('g_titlebar_cache_renders')>builds,'Resource generation did not invalidate the title'
            mouse_x=debug.read(address('g_mouse_x'),4);mouse_y=debug.read(address('g_mouse_y'),4)
            pad=max(2,title//10);button=min(title+8,(128-2*pad-4)//3)
            close_left=1168-pad-2*button-2;close_top=200-title+pad # maximize; fixture close is disabled
            debug.write(address('g_mouse_x'),(close_left+1).to_bytes(4,'little'))
            debug.write(address('g_mouse_y'),(close_top+1).to_bytes(4,'little'))
            frame()
            assert value('g_titlebar_hover_win',4)==int.from_bytes(window_id,'little')
            assert pixel_at(close_left+1,close_top+1)==0xff665078,'Hover pixels were not refreshed'
            debug.write(address('g_mouse_x'),mouse_x);debug.write(address('g_mouse_y'),mouse_y)
            debug.write(address('g_focused_win_id'),old_focus);repaint()
            builds=value('g_titlebar_cache_renders')
            debug.write(upper+x_offset,(1044).to_bytes(4,'little'));repaint()
            assert value('g_titlebar_cache_renders')==builds,'Moving a window rebuilt its title'
            debug.write(upper+x_offset,(1040).to_bytes(4,'little'));repaint()
            debug.write(upper+x_offset,upper_x);debug.write(upper+y_offset,upper_y)
            debug.write(address('g_wallpaper'),wallpaper)
            for pointer,visible in visibility:debug.write(pointer+visible_offset,visible)
            for name,number in [('g_dirty_min_x',0),('g_dirty_min_y',0),
                                ('g_dirty_max_x',1279),('g_dirty_max_y',799)]:
                debug.write(address(name),number.to_bytes(4,'little'))
            debug.write(address('g_needs_refresh'),b'\1')
            for _ in range(4):frame()
        x,y,expected=struct.unpack('<III',debug.read(address('g_gpu_test_sample'),12))
        actual=int.from_bytes(debug.read(master+y*pitch+x*4,4),'little')
        assert actual==expected,(x,y,hex(actual),hex(expected))
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
        cache_bytes=value('g_titlebar_cache_bytes')
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
        assert value('g_titlebar_cache_bytes')<cache_bytes,'Closing the window did not release its title cache'
        assert value('g_gpu_surface_epoch')>epoch,'Retired surface did not invalidate GGTT bindings'
        check_memory()
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
        if render_test:
            # Owner reproduction: medium translucent layer over fullscreen
            # animated Rainbow. Production policy must avoid RCS, retain BCS
            # animation and match transparent/half/opaque sampling every frame.
            geometry=debug.read(upper+x_offset,16)
            old_alpha=debug.read(upper+alpha_offset,1)
            old_shadow=debug.read(upper+shadow_offset,1)
            count=value('g_window_count',4)
            order=debug.read(address('g_window_order'),count*4)
            listing=struct.unpack('<'+'Q'*count,debug.read(address('g_window_list'),count*8))
            index=listing.index(upper)
            ordering=list(struct.unpack('<'+'i'*count,order));ordering.remove(index);ordering.append(index)
            debug.write(address('g_window_order'),struct.pack('<'+'i'*count,*ordering))
            buffer=int.from_bytes(debug.read(upper+buffer_offset,8),'little')
            # GDB limits packets; reuse the bounded transfer helper.
            saved=b''.join(debug.read(buffer+i,min(1024,128*96*4-i)) for i in range(0,128*96*4,1024))
            row=b''.join(struct.pack('<I',0 if x<43 else 0x80402010 if x<86 else 0xff235789) for x in range(128))
            write_surface(buffer,row*96)
            debug.write(upper+x_offset,struct.pack('<iiii',100,100,900,650))
            debug.write(upper+alpha_offset,b'\1');debug.write(upper+shadow_offset,b'\1')
            debug.write(address('g_gpu_render_hook'),render_hook)
            debug.write(address('g_cpu_alpha_pixel_limit'),(1024*1024).to_bytes(8,'little'))
            repaint()
            draws=value('g_gpu_test_render_calls');rainbows=value('g_gpu_test_rainbow_calls')
            for _ in range(4):
                frame()
                assert value('g_gpu_test_render_calls')==draws,'Medium alpha still waits for RCS'
                scroll=value('rain_scroll',4)
                for px,source in [(200,0),(550,0x80402010),(950,0xff235789)]:
                    background=int.from_bytes(debug.read(address('g_rainbow_wide')+((scroll+px)%720)*4,4),'little')
                    actual=int.from_bytes(debug.read(master+400*pitch+px*4,4),'little')
                    assert actual==over(source,background),(px,hex(actual),hex(over(source,background)))
            assert value('g_gpu_test_rainbow_calls')>rainbows,'CPU alpha stopped GPU Rainbow'
            debug.write(upper+x_offset,geometry);debug.write(upper+alpha_offset,old_alpha)
            debug.write(upper+shadow_offset,old_shadow);write_surface(buffer,saved)
            debug.write(address('g_window_order'),order)
            debug.write(address('g_cpu_alpha_pixel_limit'),bytes(8));repaint()
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
