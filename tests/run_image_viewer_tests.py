#!/usr/bin/env python3
"""Exercise the native viewer and real PNG/JPEG codec under sanitizers."""
from pathlib import Path
import subprocess
from run_image_tests import fixtures, OUT

ROOT = Path(__file__).resolve().parents[1]

def main():
    fixtures()
    binary = ROOT / '.storage-test/image-viewer-test'
    subprocess.run(['clang++', '-std=c++20', '-O1', '-g', '-no-pie',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I.',
                    'tests/image_viewer_test.cpp', 'src/gui/image.cpp', '-o', str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary), str(OUT), str(ROOT/'tests/fixtures/images/testorig.jpg'),
                    str(ROOT/'.storage-test/image-viewer.ppm')], cwd=ROOT, check=True)

if __name__ == '__main__':
    main()
