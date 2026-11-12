"""Capture and inspect desktop typography at 768p and 1080p in QEMU."""
from pathlib import Path
import struct
import subprocess
import time
import zlib
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]
symbols = subprocess.check_output(['nm', '-n', str(ROOT / 'wyzar')], text=True)
def address(name):
    return next(int(line.split()[0], 16) for line in symbols.splitlines() if line.split()[-1].endswith(name))

def png(label):
    p = ROOT / '.storage-test' / (label + '.ppm')
    _, dimensions, _, pixels = p.read_bytes().split(b'\n', 3)
    w, h = map(int, dimensions.split())
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    raw = b''.join(b'\0' + pixels[y*w*3:(y+1)*w*3] for y in range(h))
    p.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n' +
        chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
        chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
    return w, h

def main():
    smoke.IMAGE = ROOT / '.storage-test/root.img'
    for resolution in ((1024,768), (1920,1080)):
        label = f'ui-font-menu-{resolution[0]}'
        q = smoke.Qemu(resolution=resolution)
        def scalar(name):
            path = ROOT / '.storage-test/font-memory.bin'
            q.command('stop')
            try:
                assert not q.monitor(f'memsave 0x{address(name):x} 4 "{path}"')
                return int.from_bytes(path.read_bytes(), 'little', signed=True)
            finally:
                q.command('cont')
        try:
            for _ in range(40):
                time.sleep(1)
                try:
                    log = q.capture(label)
                except AssertionError:
                    continue
                if '[wm] Shell task started inside WM frame.' in log:
                    break
            else:
                raise AssertionError('Font preview did not boot')
            assert png(label) == resolution
            for _ in range(60):
                dx, dy = 45 - scalar('g_mouse_x'), resolution[1] - 20 - scalar('g_mouse_y')
                if abs(dx) <= 2 and abs(dy) <= 2:
                    break
                q.monitor(f'mouse_move {max(-80,min(80,dx))} {max(-80,min(80,dy))}')
                time.sleep(.03)
            q.monitor('mouse_button 1'); time.sleep(.1)
            q.monitor('mouse_button 0'); time.sleep(.5)
            assert scalar('g_menu_app_count') == 2
            q.capture(label)
            assert png(label) == resolution
            print(f'Font preview captured at {resolution}', flush=True)
        finally:
            q.close()

if __name__ == '__main__':
    main()
