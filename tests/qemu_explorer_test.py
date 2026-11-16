#!/usr/bin/env python3
"""Native Explorer GUI and filesystem smoke test on a disposable USB image.

Build with make iso PERSIST_DISK=.storage-test/explorer-seed.img ROOT_MIB=64.
"""
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import time
import qemu_storage_smoke as smoke
from run_explorer_tests import save_png

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'.storage-test'
OFFSET=(2048+262144)*512

def provision():
    image=OUT/'explorer-qemu.img'
    shutil.copyfile(OUT/'explorer-seed.img',image)
    obj=OUT/'trywait-probe.o';elf=OUT/'trywait-probe.elf'
    subprocess.run(['clang','-ffreestanding','-fno-stack-protector','-fno-pic','-fno-pie','-mno-red-zone',
                    '-target','x86_64-elf','-fno-builtin','-c','tests/trywait_probe.c','-o',str(obj)],cwd=ROOT,check=True)
    subprocess.run(['ld.lld','-nostdlib','-z','max-page-size=0x1000','-Ttext=0x400000','-o',str(elf),str(obj)],check=True)
    console_obj=OUT/'console-probe.o';console_elf=OUT/'console-probe.elf'
    subprocess.run(['clang','-ffreestanding','-fno-stack-protector','-fno-pic','-fno-pie','-mno-red-zone',
                    '-target','x86_64-elf','-fno-builtin','-c','tests/console_probe.c','-o',str(console_obj)],cwd=ROOT,check=True)
    subprocess.run(['ld.lld','-nostdlib','-z','max-page-size=0x1000','-Ttext=0x400000','-o',str(console_elf),str(console_obj)],check=True)
    partition=OUT/'explorer-partition.img'
    with image.open('rb') as source,partition.open('wb') as target:
        source.seek(OFFSET);shutil.copyfileobj(source,target)
    note=OUT/'explorer-note.txt';note.write_text('Hello from File Explorer.\nThis is a native text preview.\n')
    manifest=OUT/'explorer-fixtures.txt'
    manifest.write_text(f'mkdir /home/projects\nmkdir /home/archive\nput /home/readme.txt {note}\nput /bin/trywait.elf {elf}\nput /home/projects/console-probe.elf {console_elf}\n')
    subprocess.run([str(OUT/'ext4-image-tool'),str(partition),str(manifest)],check=True)
    with image.open('r+b') as target,partition.open('rb') as source:
        target.seek(OFFSET);shutil.copyfileobj(source,target)
    return image,partition

def main():
    smoke.IMAGE,partition=provision()
    symbols=subprocess.check_output(['nm','-n',str(ROOT/'wyzar')],text=True)
    def address(name):
        return int(next(line.split()[0] for line in symbols.splitlines() if line.split()[-1].endswith(name)),16)
    # Obtain the real catalog layout from its shared header, rather than assuming
    # icon/metadata structure sizes in a QMP memory dump.
    layout=OUT/'explorer-layout.cpp';binary=OUT/'explorer-layout'
    layout.write_text('#include <cstddef>\n#include <cstdio>\n#include "src/gui/resources.h"\n'
                      'int main(){printf("%zu %zu %zu",offsetof(resources::Catalog,apps),sizeof(resources::Entry),offsetof(resources::App,exec));}\n')
    subprocess.run(['clang++','-std=c++20','-I.',str(layout),'-o',str(binary)],cwd=ROOT,check=True)
    apps_offset,stride,exec_offset=map(int,subprocess.check_output([str(binary)]).split())
    q=smoke.Qemu(resolution=(1280,800))
    q.sock.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
    def memory(addr,n):
        path=OUT/'explorer-memory.bin';q.command('stop')
        try:
            assert not q.monitor(f'memsave 0x{addr:x} {n} "{path}"')
            return path.read_bytes()
        finally:q.command('cont')
    def scalar(name,size=4):return int.from_bytes(memory(address(name),size),'little')
    def windows():
        count=scalar('g_window_count');pointers=memory(address('g_window_list'),count*8)
        result={}
        for (pointer,) in struct.iter_unpack('<Q',pointers):
            data=memory(pointer,384);title=data[4:68].split(b'\0')[0].decode()
            result[title]=(pointer,struct.unpack_from('<iiii',data,324))
        return result
    def move(x,y):
        for _ in range(60):
            dx=x-scalar('g_mouse_x');dy=y-scalar('g_mouse_y')
            if abs(dx)<=1 and abs(dy)<=1:return
            q.monitor(f'mouse_move {max(-90,min(90,dx))} {max(-90,min(90,dy))}');time.sleep(.025)
        raise AssertionError('Mouse could not reach target')
    def click(x,y):
        move(x,y);q.monitor('mouse_button 1');time.sleep(.07);q.monitor('mouse_button 0');time.sleep(.2)
    def double(x,y):click(x,y);click(x,y);time.sleep(.4)
    def client(x,y):click(130+x,100+y)
    def drag(title,edge,dx,dy):
        focus(title);_,(x,y,w,h)=windows()[title];th=scalar('g_title_h')
        sx=x+2 if 'l' in edge else x+w-2 if 'r' in edge else x+w//2
        sy=y-th+2 if 't' in edge else y+h-2 if 'b' in edge else y+h//2
        move(sx,sy);q.monitor('mouse_button 1');time.sleep(.1);move(sx+dx,sy+dy)
        q.monitor('mouse_button 0');time.sleep(.5)
        return windows()[title][1]
    def go(path):
        client(300,26);q.type(path+'\n',pause=.4)
    def terminal():
        data=memory(address('g_term_matrix'),640000)[::8]
        return '\n'.join(data[i:i+160].decode('latin1').rstrip('\0 ') for i in range(0,len(data),160))
    def capture(label):
        q.command('screendump',{'filename':str(OUT/(label+'.ppm'))});save_png(OUT/(label+'.ppm'),OUT/(label+'.png'))
    def focus(title):
        pointer,_=windows()[title]
        if scalar('g_focused_win_id')==int.from_bytes(memory(pointer,4),'little'):return
        index=list(windows()).index(title);width=scalar('g_wbtn_w');gap=800*4//1080
        click(6+scalar('g_sb_w')+800*10//1080+index*(width+gap)+width//2,780)
    def control(title,which):
        focus(title);_,(x,y,w,h)=windows()[title];th=scalar('g_title_h');pad=max(2,th//10);bw=th+8
        click(x+w-pad-(which+1)*bw-which*2+bw//2,y-th+th//2);time.sleep(.3)
    def window_controls(title):
        focus(title);pointer,original=windows()[title]
        count=len(windows());identity=int.from_bytes(memory(pointer,4),'little')
        control(title,2)
        assert scalar('g_focused_win_id')!=identity,(title,'minimize retained focus')
        assert len(windows())==count,(title,'minimize destroyed window')
        click(original[0]+original[2]//2,original[1]+original[3]//2)
        assert scalar('g_focused_win_id')!=identity,(title,'minimized window still receives clicks')
        capture('minimized-'+title.split()[0].lower())
        focus(title);assert windows()[title][1]==original,(title,'minimize changed bounds')
        control(title,1)
        expected=(0,scalar('g_title_h'),1280,800-scalar('g_title_h')-scalar('g_taskbar_h'))
        assert windows()[title][1]==expected,(title,windows()[title][1],expected)
        capture('maximized-'+title.split()[0].lower())
        control(title,1);assert windows()[title][1]==original,(title,'restore changed bounds')
    font_source=(ROOT/'src/gui/font8x8_basic.h').read_text()
    glyphs=[]
    for body in re.findall(r'\{([^{}]+)\}',font_source):
        values=re.findall(r'0x([0-9a-fA-F]{2})\b',body)
        if len(values)==8:glyphs.append(bytes(int(v,16) for v in values))
    font_lookup={glyphs[c]:chr(c) for c in range(32,127)}
    def console_text(title):
        pointer,(_,_,width,height)=windows()[title]
        buffer=struct.unpack_from('<Q',memory(pointer,384),344)[0]
        data=memory(buffer,width*height*4);pixels=memoryview(data).cast('I');lines=[]
        for row in range((height-28-40)//10):
            line=''
            for col in range((width-24)//8):
                glyph=bytes(sum((1<<x) if pixels[(40+row*10+y)*width+12+col*8+x]==0xffffffff else 0 for x in range(8)) for y in range(8))
                line+=font_lookup.get(glyph,'?')
            lines.append(line.rstrip())
        return '\n'.join(lines)
    try:
        log=''
        for _ in range(80):
            time.sleep(1)
            try:log=q.capture('explorer-boot')
            except AssertionError as error:
                if 'Invalid addr' in str(error):continue
                raise
            if '[init] Decoupled terminal boot.' in log:break
        else:raise AssertionError('Boot timeout: '+log[-5000:])
        time.sleep(2)
        q.type('run /bin/trywait.elf\n',pause=4)
        assert '[trywait] passed running, completion and repeat checks' in terminal(),terminal()
        print('TRYWAIT real syscall passed',flush=True)
        mouse=next(m for m in q.command('query-mice') if 'USB' in m['name'] or 'HID' in m['name'])
        q.monitor(f"mouse_set {mouse['index']}")
        click(30,780);time.sleep(.4)
        catalog=scalar('g_resources_catalog',8);count=int.from_bytes(memory(catalog,4),'little')
        data=memory(catalog+apps_offset,count*stride)
        paths=[data[i*stride+exec_offset:i*stride+exec_offset+256].split(b'\0')[0].decode() for i in range(count)]
        index=paths.index('/bin/explorer.wiz')
        hdr=max(44,800*56//1080);row=max(26,800*38//1080);pad=max(10,800*14//1080)
        top=800-scalar('g_taskbar_h')-min(hdr+max(1,count)*row+2*pad+max(52,800*64//1080),800*740//1080)-800*4//1080
        click(80,top+hdr+pad+index*row+row//2);time.sleep(1)
        assert 'File Explorer' in windows(),windows()
        assert terminal().rstrip().endswith('wyzar:/>'),terminal()
        capture('explorer-home')
        print('Start-menu launch and initial render passed',flush=True)
        original=windows()['File Explorer'][1]
        grown=drag('File Explorer','rb',100,70);assert grown[2:]==(960,630),grown
        pointer,_=windows()['File Explorer'];assert struct.unpack_from('<I',memory(pointer,384),352)[0]==960*4
        capture('explorer-resize-large')
        shrunk=drag('File Explorer','rb',-700,-500);assert shrunk[2:]==(600,360),shrunk
        capture('explorer-resize-small')
        restored=drag('File Explorer','rb',260,200);assert restored==original,restored
        corner=drag('File Explorer','lt',32,24);assert corner==(162,124,828,536),corner
        restored=drag('File Explorer','lt',-32,-24);assert restored==original,restored
        window_controls('File Explorer')
        # An active taskbar button toggles minimization without closing the app.
        pointer,_=windows()['File Explorer'];identity=int.from_bytes(memory(pointer,4),'little')
        index=list(windows()).index('File Explorer');gap=800*4//1080
        click(6+scalar('g_sb_w')+800*10//1080+index*(scalar('g_wbtn_w')+gap)+scalar('g_wbtn_w')//2,780)
        assert scalar('g_focused_win_id')!=identity
        focus('File Explorer');assert windows()['File Explorer'][1]==original
        # A title-bar double-click uses the same maximize/restore path.
        for maximized in (True,False):
            _,(x,y,w,h)=windows()['File Explorer'];move(x+100,y-scalar('g_title_h')//2)
            for _ in range(2):
                q.monitor('mouse_button 1');time.sleep(.04);q.monitor('mouse_button 0');time.sleep(.06)
            time.sleep(.4)
            actual=windows()['File Explorer'][1]
            assert actual[2]==1280 if maximized else actual==original,actual
        _,(x,y,w,h)=windows()['File Explorer']
        for label,mx,my in [('horizontal',x+2,y+h//2),('diagonal-up',x+2,y+h-2),('diagonal-down',x+w-2,y+h-2)]:
            move(mx,my);time.sleep(.2);capture('cursor-'+label)
        # Every built-in window supports resizing at native font/pixel sizes.
        for title in ('Terminal','Boot Log','Visual Test','Rainbow Animation'):
            window_controls(title)
            before=windows()[title][1];after=drag(title,'l',32,0)
            assert after[2]<before[2],(title,before,after)
            pointer,_=windows()[title];assert struct.unpack_from('<I',memory(pointer,384),352)[0]==after[2]*4
        focus('Terminal');q.type('write /home/resize-ok.txt resized\n',pause=.3)
        focus('File Explorer')
        print('All built-in windows and Explorer resize, minimize and maximize/restore correctly',flush=True)
        # Home: Applications, archive, projects, readme.txt (folders first).
        double(130+265,100+138+3*34+16);assert 'Notepad' in windows(),windows()
        capture('explorer-notepad')
        _,(nx,ny,_,_)=windows()['Notepad']
        click(nx+25,ny+15);capture('notepad-file-menu')
        click(nx+60,ny+83);assert 'Open' in windows(),windows()
        capture('notepad-open-browser');control('Open',0);assert 'Open' not in windows()
        focus('Notepad');q.monitor('sendkey ctrl-shift-s 40');time.sleep(.5)
        assert 'Save As' in windows(),windows()
        capture('notepad-save-browser');control('Save As',0);assert 'Save As' not in windows()
        control('Notepad',0);assert 'Notepad' not in windows()
        focus('File Explorer')
        client(214,77);q.type('created\n',pause=.3) # New folder dialog, initial name selected.
        go('/home/created');client(300,77);q.type('scratch.txt\n',pause=.3)
        client(260,155);client(374,77);q.type('renamed.txt\n',pause=.3)
        client(260,155);client(432,77);go('/home/archive');client(545,77)
        # Copy the text file and move a directory through the real GUI actions.
        go('/home');client(260,138+4*34+15);client(432,77);go('/home/archive');client(545,77)
        go('/home');client(260,138+3*34+15);client(490,77);go('/home/archive');client(545,77)
        go('/share/samples');double(130+265,100+138+16);time.sleep(.5) # bundle_demo.wiz
        assert 'Console - bundle_demo.wiz' in windows(),windows()
        assert 'This is run number 1' in console_text('Console - bundle_demo.wiz'),console_text('Console - bundle_demo.wiz')
        assert 'This is run number 1' not in terminal(),terminal()
        size=drag('Console - bundle_demo.wiz','rb',100,70);assert size[2:]==(844,510),size
        assert 'This is run number 1' in console_text('Console - bundle_demo.wiz')
        capture('console-resize-large')
        window_controls('Console - bundle_demo.wiz')
        assert 'This is run number 1' in console_text('Console - bundle_demo.wiz')
        focus('File Explorer')
        client(670,77);capture('explorer-package-install');q.type('\n',pause=.2)
        client(260,138+34+16);client(730,77);q.type('\n',pause=.2) # pin counter.wiz
        print('Double-click package launch passed',flush=True)
        go('/home/archive/projects');double(130+265,100+138+16);time.sleep(.3)
        title='Console - console-probe.elf';assert title in windows(),windows()
        assert 'input ready' in console_text(title),console_text(title)
        focus('Terminal');q.type('pwd\n',pause=.2)
        assert terminal().rstrip().endswith('wyzar:/>') and 'File Explorer' in windows(),terminal()
        focus(title);q.type('own-input\n',pause=.3)
        assert 'received: own-input' in console_text(title),console_text(title)
        q.monitor('sendkey ctrl-c 40');time.sleep(.6)
        assert 'Process exited with code 130.' in console_text(title),console_text(title)
        assert 'File Explorer' in windows(),windows()
        capture('explorer-private-console')
        _,(cx,cy,cw,ch)=windows()[title];th=scalar('g_title_h');click(cx+cw-th//2,cy-th//2)
        focus('File Explorer');go('/share/samples')
        print('Private console input/output, shell independence and scoped Ctrl+C passed',flush=True)
        # A GUI child must not block Explorer; close it and keep browsing.
        double(130+265,100+138+3*34+16);time.sleep(.5) # win_test.wiz
        child=windows().get('Wyzar Desktop Demo');assert child,windows()
        window_controls('Wyzar Desktop Demo')
        old_pitch=struct.unpack_from('<I',memory(child[0],384),352)[0]
        size=drag('Wyzar Desktop Demo','rb',100,60);assert size[2:]==(580,380),size
        pointer,_=windows()['Wyzar Desktop Demo'];assert struct.unpack_from('<I',memory(pointer,384),352)[0]==old_pitch
        q.type('r',pause=.1);click(size[0]+size[2]*3//4,size[1]+size[3]*3//5)
        buffer=struct.unpack_from('<Q',memory(pointer,384),344)[0]
        pixel=memory(buffer+(192*480+360)*4,4)
        assert int.from_bytes(pixel,'little')==0xffff3333,pixel
        capture('legacy-window-resize')
        child=windows()['Wyzar Desktop Demo']
        _,(x,y,w,h)=child;title_h=scalar('g_title_h');click(x+w-title_h//2,y-title_h//2);time.sleep(.5)
        assert 'Wyzar Desktop Demo' not in windows(),windows()
        go('/home/created');client(260,155);client(607,77);q.type('\n',pause=.3)
        go('/home');capture('explorer-complete')
        pointer,(x,y,w,h)=windows()['File Explorer'];click(x+w-title_h//2,y-title_h//2);time.sleep(.4)
        assert 'File Explorer' not in windows(),windows()
        assert terminal().rstrip().endswith('wyzar:/>'),terminal()
        focus('Terminal');q.type('run /bin/explorer.wiz\n',pause=.5)
        assert 'File Explorer' in windows() and terminal().rstrip().endswith('wyzar:/>'),terminal()
        focus('Terminal');q.monitor('sendkey ctrl-c 40');q.type('pwd\n',pause=.2)
        assert 'File Explorer' in windows(),windows()
        focus('File Explorer');q.monitor('sendkey ctrl-c 40');time.sleep(.2)
        assert 'File Explorer' in windows(),windows()
        _,(x,y,w,h)=windows()['File Explorer'];click(x+w-title_h//2,y-title_h//2)
        print('Shell-launched GUI returns its prompt and survives unrelated Ctrl+C',flush=True)
        print('GUI navigation, dialogs, file operations, child windows and close passed',flush=True)
    finally:q.close()
    with smoke.IMAGE.open('rb') as source,partition.open('wb') as target:
        source.seek(OFFSET);shutil.copyfileobj(source,target)
    checked=subprocess.run(['e2fsck','-p',str(partition)],capture_output=True)
    assert checked.returncode in (0,1),checked.stdout+checked.stderr
    subprocess.run(['e2fsck','-fn',str(partition)],capture_output=True,check=True)
    def read(path):return subprocess.check_output(['debugfs','-R','cat '+path,str(partition)],stderr=subprocess.DEVNULL)
    assert read('/home/archive/readme.txt')==b'Hello from File Explorer.\nThis is a native text preview.\n'
    created=subprocess.check_output(['debugfs','-R','stat /home/archive/renamed.txt',str(partition)],stderr=subprocess.STDOUT)
    assert b'Inode:' in created and b'Size: 0' in created,created
    moved=subprocess.check_output(['debugfs','-R','stat /home/archive/projects',str(partition)],stderr=subprocess.STDOUT)
    assert b'Inode:' in moved and b'directory' in moved,moved
    missing=subprocess.check_output(['debugfs','-R','stat /home/created/renamed.txt',str(partition)],stderr=subprocess.STDOUT)
    assert b'File not found' in missing,missing
    assert read('/home/.local/share/bundle-demo/runs.txt')==b'1'
    assert read('/apps/bundle-demo/bundle-demo.wiz')==(ROOT/'build/packages/bundle_demo.wiz').read_bytes()
    assert b'kind=pinned' in read('/apps/counter/app.manifest')
    assert read('/home/resize-ok.txt')==b'resized'
    print('Explorer QEMU disk verification and filesystem checker passed',flush=True)

if __name__=='__main__':main()
