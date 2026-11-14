#!/usr/bin/env python3
"""Native Explorer GUI and filesystem smoke test on a disposable USB image.

Build with make iso PERSIST_DISK=.storage-test/explorer-seed.img ROOT_MIB=64.
"""
from pathlib import Path
import re
import shutil
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
    def go(path):
        client(300,26);q.type(path+'\n',pause=.4)
    def terminal():
        data=memory(address('g_term_matrix'),640000)[::8]
        return '\n'.join(data[i:i+160].decode('latin1').rstrip('\0 ') for i in range(0,len(data),160))
    def capture(label):
        q.command('screendump',{'filename':str(OUT/(label+'.ppm'))});save_png(OUT/(label+'.ppm'),OUT/(label+'.png'))
    def focus(title):
        index=list(windows()).index(title);width=scalar('g_wbtn_w');gap=800*4//1080
        click(6+scalar('g_sb_w')+800*10//1080+index*(width+gap)+width//2,780)
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
            for col in range(88):
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
        hdr=max(24,800*36//1080);row=max(26,800*38//1080);pad=max(6,800*10//1080)
        top=800-scalar('g_taskbar_h')-(hdr+count*row+2*pad)-800*4//1080
        click(80,top+hdr+pad+index*row+row//2);time.sleep(1)
        assert 'File Explorer' in windows(),windows()
        assert terminal().rstrip().endswith('wyzar:/>'),terminal()
        capture('explorer-home')
        print('Start-menu launch and initial render passed',flush=True)
        # Home: Applications, archive, projects, readme.txt (folders first).
        double(130+265,100+138+3*34+16);capture('explorer-preview');q.type('\n',pause=.2)
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
    print('Explorer QEMU disk verification and filesystem checker passed',flush=True)

if __name__=='__main__':main()
