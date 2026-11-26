#!/usr/bin/env python3
"""Run the actual native MP3 app with a deterministic syscall/audio backend."""
from pathlib import Path
import subprocess
from run_explorer_tests import save_png

ROOT = Path(__file__).resolve().parents[1]

def main():
    out = ROOT / '.storage-test'
    out.mkdir(exist_ok=True)
    binary = out / 'music-player-test'
    subprocess.run(['clang++', '-std=c++20', '-O1', '-g', '-no-pie',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I.',
                    'tests/music_player_test.cpp', '-o', str(binary)], cwd=ROOT, check=True)
    subprocess.run([str(binary), str(out/'music-player.ppm')], cwd=ROOT, check=True)
    save_png(out/'music-player.ppm', out/'music-player.png')

if __name__ == '__main__':
    main()
