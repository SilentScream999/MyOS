#!/usr/bin/env python3
from pathlib import Path
import subprocess
from run_explorer_tests import save_png

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.storage-test'

def main():
    OUT.mkdir(exist_ok=True)
    binary = OUT / 'notepad-test'
    subprocess.run(['clang++', '-std=c++20', '-O1', '-g', '-no-pie',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I.',
                    'tests/notepad_test.cpp', '-o', str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary), str(OUT / 'notepad-host.ppm')], cwd=ROOT, check=True)
    save_png(OUT / 'notepad-host.ppm', OUT / 'notepad-host.png')
    for ppm in OUT.glob('notepad-host.ppm-*.ppm'):
        save_png(ppm, ppm.with_suffix('.png'))

if __name__ == '__main__':
    main()
