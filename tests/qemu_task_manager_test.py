"""Real process category metadata, CPU graph UI, and collapsible sections."""
from pathlib import Path
import argparse
import struct
import subprocess
import sys
import time
import qemu_storage_smoke as smoke
from run_explorer_tests import save_png

ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser();parser.add_argument('--ps2',action='store_true');args=parser.parse_args()
sys.path.insert(0,str(ROOT/'tools'))
from image_io import copy_sparse
image=smoke.OUT/'task-manager-test.img';partition=smoke.OUT/'task-manager-partition.img'
with (smoke.OUT/'root.img').open('rb') as source,image.open('wb') as target:
    copy_sparse(source,target,(smoke.OUT/'root.img').stat().st_size)
with image.open('rb') as source,partition.open('wb') as target:
    mbr=source.read(512);start,sectors=struct.unpack_from('<II',mbr,446+16+8)
    source.seek(start*512);copy_sparse(source,target,sectors*512)
obj=smoke.OUT/'process-metadata.o';elf=smoke.OUT/'process-metadata.elf'
subprocess.run(['clang++','-std=c++20','-O0','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic','-fno-pie',
    '-mno-red-zone','-target','x86_64-elf','-I.','-c','tests/process_metadata_probe.cpp','-o',str(obj)],check=True)
subprocess.run(['ld.lld','-nostdlib','-z','max-page-size=0x1000','-Ttext=0x400000','-o',str(elf),str(obj)],check=True)
manifest=smoke.OUT/'process-metadata-files.txt';manifest.write_text(f'put /bin/process-metadata.elf {elf}\n')
subprocess.run([str(smoke.OUT/'ext4-image-tool'),str(partition),str(manifest)],check=True)
with image.open('r+b') as target,partition.open('rb') as source:
    target.seek(start*512);copy_sparse(source,target,sectors*512)
symbols=subprocess.check_output(['nm','-n','-C',str(ROOT/'wyzar')],text=True).splitlines()
def address(name):return next(int(l.split()[0],16) for l in symbols if l.split()[-1]==name)
smoke.IMAGE=image;smoke.BOOT_ISO=True
q=smoke.Qemu(resolution=(1280,800),input_devices=not args.ps2,
    extra_args=('-device','usb-mouse') if args.ps2 else ())
try:
    def memory(p,n):
        path=smoke.OUT/'task-manager-state.bin';assert not q.monitor(f'memsave 0x{p:x} {n} "{path}"');return path.read_bytes()
    def value(name,n=4):return int.from_bytes(memory(address(name),n),'little')
    def terminal():return memory(address('g_term_matrix'),640000)[::8].replace(b'\0',b' ').decode(errors='replace')
    def capture(label):
        q.capture(label);save_png(smoke.OUT/(label+'.ppm'),smoke.OUT/(label+'.png'))
        raw=(smoke.OUT/(label+'.ppm')).read_bytes();return raw.split(b'\n',3)[3]
    def move(x,y):
        for _ in range(60):
            dx=x-value('g_mouse_x');dy=y-value('g_mouse_y')
            if not dx and not dy:return
            q.monitor(f'mouse_move {max(-80,min(80,dx))} {max(-80,min(80,dy))}');time.sleep(.05)
        raise AssertionError('Pointer target not reached')
    def click(x,y):
        move(x,y);q.monitor('mouse_button 1');time.sleep(.15);q.monitor('mouse_button 0');time.sleep(.4)
    def wait_for(condition,message):
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            if condition():return
            time.sleep(.1)
        raise AssertionError(message)
    for _ in range(120):
        time.sleep(.5)
        try:log=q.capture('task-manager-boot')
        except AssertionError:continue
        if '[wm] Shell task started inside WM frame.' in log:break
    else:raise AssertionError('boot timeout')
    q.type('run /bin/task_manager.wiz\n',pause=1)
    q.type('run /bin/process-metadata.elf\n',pause=1)
    assert '[process metadata] passed' in terminal(),terminal()
    time.sleep(2);click(200,130);capture('task-manager-programs')
    click(200,364) # Programs header: window client y=100 plus table y=254.
    raw=capture('task-manager-collapsed')
    assert raw[(389*1280+150)*3:(389*1280+150)*3+3]==bytes.fromhex('292438')
    click(200,364);raw=capture('task-manager-expanded')
    assert raw[(389*1280+150)*3:(389*1280+150)*3+3]==bytes.fromhex('201d2d')
    assert value('g_kernel_cpu_until_tick',8)==0,'Process view unexpectedly enabled kernel profiling'
    # Global shortcut reuses a running instance and restores it from minimized.
    manager_id=value('g_focused_win_id');window_count=value('g_window_count');text=terminal()
    title=value('g_title_h');pad=max(2,title//10);button_width=title+8
    click(900-pad-3*button_width-4+button_width//2,100-title//2)
    assert value('g_desktop_shortcut_hook',8)!=0
    result=q.monitor('sendkey ctrl-shift-esc');assert not result,result
    wait_for(lambda:value('g_focused_win_id')==manager_id,'Shortcut did not restore/focus Task Manager')
    capture('task-manager-shortcut-restore')
    assert value('g_window_count')==window_count and value('g_focused_win_id')==manager_id
    wait_for(lambda:value('g_kbd_modifiers')==0,'Shortcut left Ctrl/Shift modifiers stuck')
    assert terminal()==text,'Shortcut leaked Escape into the shared shell'
    # Close it, then launch globally from the unfocused desktop. Held keys must
    # not queue new instances, and the desktop launch must focus its first window.
    click(900-pad-button_width//2,100-title//2)
    wait_for(lambda:value('g_window_count')==window_count-1,'Task Manager did not close')
    click(30,120);result=q.monitor('sendkey ctrl-shift-esc 1200');assert not result,result
    wait_for(lambda:value('g_window_count')==window_count,'Global shortcut did not launch Task Manager')
    wait_for(lambda:value('g_focused_win_id') not in (manager_id,0xffffffff),'Shortcut launch did not focus its window')
    time.sleep(2)
    wait_for(lambda:value('g_kbd_modifiers')==0,'Held shortcut modifiers did not release')
    assert value('g_window_count')==window_count
    assert value('g_focused_win_id') not in (manager_id,0xffffffff)
    capture('task-manager-shortcut')
    assert 'KERNEL PANIC' not in q.capture('task-manager-passed')
    print('Task Manager QEMU passed: categories/metadata, section controls, Ctrl+Shift+Esc launch/restore/focus, no held-key duplicates or shell leakage.',flush=True)
finally:q.close()
