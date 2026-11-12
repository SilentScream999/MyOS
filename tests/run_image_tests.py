#!/usr/bin/env python3
"""Generate independent PNG fixtures; run actual codecs/resources with ASan/UBSan."""
from pathlib import Path
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.storage-test/image-fixtures'


def chunk(tag, data):
    return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag+data))


def png(w, h, depth, color, raw, extra=b'', interlace=0):
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w,h,depth,color,0,0,interlace))
            + extra + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def fixtures():
    OUT.mkdir(parents=True, exist_ok=True)
    cases = []

    def save(name, data, w, h, expected):
        (OUT / f'{name}.png').write_bytes(data)
        (OUT / f'{name}.rgba').write_bytes(bytes(expected))
        cases.append(f'{name} {w} {h}')

    save('alpha', png(2,1,8,6,b'\0\xff\0\0\x80\0\xff\0\0'),2,1,[255,0,0,128,0,255,0,0])
    save('blue', png(2,1,8,6,b'\0\0\0\xff\xff\0\0\xff\xff'),2,1,[0,0,255,255]*2)
    save('rgb',png(2,1,8,2,b'\0\x0a\x14\x1e\x28\x32\x3c'),2,1,[10,20,30,255,40,50,60,255])
    save('gray-alpha',png(2,1,8,4,b'\0\x40\x80\xc0\xff'),2,1,[64,64,64,128,192,192,192,255])
    for depth in (1,2,4,8):
        values=list(range(1<<min(depth,4)))
        samples=[v*((1<<depth)-1)//(len(values)-1) for v in values]
        packed=bytearray((len(samples)*depth+7)//8)
        for i,v in enumerate(samples): packed[i*depth//8] |= v << (8-depth-(i*depth%8))
        expected=[]
        for v in samples: expected.extend([v*255//((1<<depth)-1)]*3+[255])
        save(f'gray-{depth}',png(len(samples),1,depth,0,b'\0'+packed),len(samples),1,expected)
    palette=chunk(b'PLTE',bytes([255,0,0,0,255,0]))+chunk(b'tRNS',bytes([128,255]))
    save('palette',png(2,1,1,3,b'\0\x40',palette),2,1,[255,0,0,128,0,255,0,255])
    save('rgb-transparent',png(2,1,8,2,b'\0\x0a\x14\x1e\x28\x32\x3c',chunk(b'tRNS',struct.pack('>HHH',10,20,30))),2,1,[10,20,30,0,40,50,60,255])
    save('rgba16',png(1,1,16,6,b'\0'+struct.pack('>HHHH',0x1234,0x5678,0x9abc,0x8000)),1,1,[0x12,0x56,0x9a,0x80])
    rows=[bytes((x*19+y*31)%256 for x in range(12)) for y in range(5)]
    for filter_type in range(5):
        raw=bytearray()
        for y,row in enumerate(rows):
            raw.append(filter_type)
            for x,value in enumerate(row):
                a=row[x-4] if x>=4 else 0
                b=rows[y-1][x] if y else 0
                c=rows[y-1][x-4] if y and x>=4 else 0
                p=a+b-c
                pa,pb,pc=abs(p-a),abs(p-b),abs(p-c)
                prediction=[0,a,b,(a+b)//2,a if pa<=pb and pa<=pc else b if pb<=pc else c][filter_type]
                raw.append((value-prediction)%256)
        save(f'filter-{filter_type}',png(3,5,8,6,raw),3,5,b''.join(rows))
    w,h=9,9
    pixels=bytes(v for y in range(h) for x in range(w) for v in (x*25,y*25,100,(x+y)*15))
    raw=bytearray()
    for x0,y0,dx,dy in ((0,0,8,8),(4,0,8,8),(0,4,4,8),(2,0,4,4),(0,2,2,4),(1,0,2,2),(0,1,1,2)):
        for y in range(y0,h,dy):
            raw.append(0)
            for x in range(x0,w,dx): raw.extend(pixels[(y*w+x)*4:(y*w+x+1)*4])
    save('adam7',png(w,h,8,6,raw,interlace=1),w,h,pixels)
    (OUT/'cases.txt').write_text('\n'.join(cases)+'\n')


def main():
    fixtures()
    flags=['clang++','-std=c++20','-O1','-g','-no-pie','-fsanitize=address,undefined','-fno-omit-frame-pointer']
    for name in ('image','resources'):
        executable=ROOT/f'.storage-test/{name}-test'
        subprocess.run(flags+[f'tests/{name}_test.cpp','src/gui/image.cpp','-o',str(executable)],cwd=ROOT,check=True)
        subprocess.run([str(executable),str(OUT),str(ROOT/'tests/fixtures/images/testorig.jpg')],cwd=ROOT,check=True)
    recovery=ROOT/'.storage-test/recovery-test'
    subprocess.run(flags+['-iquote','src/core','-iquote','src/drivers','tests/recovery_test.cpp','-o',str(recovery)],cwd=ROOT,check=True)
    subprocess.run([str(recovery)],cwd=ROOT,check=True)
    print('Image, lazy resource and recovery lifecycle sanitizer tests passed.',flush=True)


if __name__ == '__main__':
    main()
