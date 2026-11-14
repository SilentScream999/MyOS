#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile
import struct
import zlib

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'.storage-test'

def save_png(ppm, output):
    magic, dimensions, maximum, data=Path(ppm).read_bytes().split(b'\n',3)
    assert magic==b'P6' and maximum==b'255'
    w,h=map(int,dimensions.split())
    def chunk(tag,value):
        return struct.pack('>I',len(value))+tag+value+struct.pack('>I',zlib.crc32(tag+value))
    raw=b''.join(b'\0'+data[y*w*3:(y+1)*w*3] for y in range(h))
    Path(output).write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+
                             chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))

def main():
    OUT.mkdir(exist_ok=True)
    binary=OUT/'explorer-test'
    subprocess.run(['clang++','-std=c++20','-O1','-g','-no-pie','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-I.','tests/explorer_test.cpp','-o',str(binary)],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='wyzar-explorer-') as tmp:
        subprocess.run([str(binary),tmp,str(OUT/'explorer-host.ppm')],cwd=ROOT,check=True)
    save_png(OUT/'explorer-host.ppm',OUT/'explorer-host.png')

if __name__=='__main__':
    main()
