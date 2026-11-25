#!/usr/bin/env python3
"""Validate/read a Wyzar Fn capture; optionally extract its ACPI AML tables."""
import argparse
from pathlib import Path
import struct

STAGES=['Fn+F1','Fn+F2','Fn+F3','Fn+Esc','Fn+F1 after FnLock','Fn+F2 after FnLock','Fn+F3 after FnLock','plain F1','plain F2','plain F3']
KINDS=['PS2','NVRAM(CMOS 0x6e)','EC event flags','EC audio 0x30','STAGE','EC read error','SCI_EN','EC mode 0x03']

def decode(data):
    if len(data)<32:raise ValueError('Truncated capture')
    magic,length,count=struct.unpack_from('<8sII',data)
    if magic!=b'WFNKEY1\0' or length<16 or length>4112 or count>35 or 16+length>len(data):raise ValueError('Invalid capture header')
    version,events,dropped,flags=struct.unpack_from('<IIII',data,16)
    if version!=1 or events>512 or length!=16+events*8:raise ValueError('Invalid trace bounds/version')
    trace=[struct.unpack_from('<IHBB',data,32+i*8) for i in range(events)]
    tables={};offset=16+length
    for _ in range(count):
        if offset+8>len(data):raise ValueError('Missing table header')
        index,size=struct.unpack_from('<II',data,offset);offset+=8
        if index>=35 or index in tables or size<36 or size>1024*1024 or offset+size>len(data):raise ValueError('Invalid table bounds/index')
        table=data[offset:offset+size];offset+=size
        signature=b'FACP' if index==0 else b'ECDT' if index==1 else b'DSDT' if index==2 else b'SSDT'
        if table[:4]!=signature or struct.unpack_from('<I',table,4)[0]!=size or sum(table)&255:raise ValueError('Bad table signature/length/checksum')
        tables[index]=table
    if offset!=len(data):raise ValueError('Unexpected trailing data')
    return trace,dropped,flags,tables

def main():
    parser=argparse.ArgumentParser();parser.add_argument('capture',type=Path);parser.add_argument('--extract-dir',type=Path);args=parser.parse_args()
    trace,dropped,flags,tables=decode(args.capture.read_bytes())
    print(f'E470 identity={bool(flags&1)} EC resources={bool(flags&2)} events={len(trace)} dropped={dropped}')
    for milliseconds,value,kind,stage in trace:
        label=STAGES[stage] if stage<len(STAGES) else str(stage)
        print(f'{milliseconds:5}ms {label:24} {KINDS[kind] if kind<len(KINDS) else kind}: 0x{value:04x}')
    if args.extract_dir:args.extract_dir.mkdir(parents=True,exist_ok=True)
    for index,table in tables.items():
        name=table[:4].decode()+(str(index-3) if index>=3 else '')+'.aml'
        print(f'{name}: {len(table)} bytes, OEM {table[10:16].decode(errors="replace")}')
        if args.extract_dir:(args.extract_dir/name).write_bytes(table)

if __name__=='__main__':main()
