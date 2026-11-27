#!/usr/bin/env python3
"""Verify the diagnostic on the persistent USB root, not only the ISO initrd.

Reads an image file only; extracts a private partition copy for Linux debugfs.
Run after make iso / make usb-image, under WSL/Linux.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'tools'))
from make_usb_image import validate_existing,START,BOOT_SECTORS,SECTOR

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image',type=Path,nargs='?',default=ROOT/'wyzar-usb.img')
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='wyzar-power-image-') as temporary:
        partition=Path(temporary)/'root.img'
        with args.image.open('rb') as source,partition.open('wb') as destination:
            sectors=validate_existing(source)
            source.seek((START+BOOT_SECTORS)*SECTOR)
            remaining=sectors*SECTOR
            while remaining:
                data=source.read(min(1024*1024,remaining))
                assert data,'Truncated USB system partition'
                destination.write(data);remaining-=len(data)
        # Confirm the exact built bytes, including the Console version that
        # launches this helper. debugfs can return zero for missing files.
        for guest,host in (('/share/system/power_diag.elf',ROOT/'power_diag.elf'),
                           ('/bin/init.elf',ROOT/'init.elf')):
            content=subprocess.check_output(['debugfs','-R','cat '+guest,str(partition)],stderr=subprocess.DEVNULL)
            assert content==host.read_bytes(),f'{guest} missing or stale in {args.image}'
            assert content.startswith(b'\x7fELF'),guest
            print('PASS: USB persistent root contains current '+guest,flush=True)

if __name__=='__main__':main()
