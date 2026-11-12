#!/usr/bin/env python3
"""Author Wyzar's geometric PNG icon set. Optional artwork tool, never a build step.

The generated PNGs are checked-in runtime resources. Installed applications and
themes supply ordinary PNG files and never need this tool or an OS rebuild.
"""
from pathlib import Path
import math
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1] / 'assets/icons/hicolor'


def png(width, height, rgba):
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))
    rows = b''.join(b'\0' + rgba[y*width*4:(y+1)*width*4] for y in range(height))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


def distance(x, y, line):
    ax, ay, bx, by = line
    dx, dy = bx-ax, by-ay
    t = max(0, min(1, ((x-ax)*dx+(y-ay)*dy)/(dx*dx+dy*dy)))
    return math.hypot(x-ax-t*dx, y-ay-t*dy)


def icon(name, size):
    colors = {'terminal': (59, 116, 195), 'application-default': (81, 116, 198),
              'visual-test': (39, 153, 151), 'rainbow': (143, 89, 191),
              'boot-log': (76, 132, 159), 'counter': (194, 126, 47), 'win_test': (39, 153, 151)}
    lines = {
        'terminal': [(19, 23, 28, 31), (28, 31, 19, 39), (34, 40, 45, 40)],
        'boot-log': [(22, 21, 42, 21), (22, 29, 42, 29), (22, 37, 42, 37), (22, 45, 34, 45)],
        'counter': [(23, 21, 23, 43), (32, 21, 32, 43), (20, 28, 44, 28), (20, 36, 44, 36)],
        'application-default': [(20, 21, 44, 21), (20, 21, 20, 43), (44, 21, 44, 43),
                                (20, 43, 44, 43), (20, 27, 44, 27)],
    }
    pixels = bytearray()
    for py in range(size):
        for px in range(size):
            sums = [0, 0, 0, 0]
            for sy in range(4):
                for sx in range(4):
                    x, y = (px+(sx+.5)/4)*64/size, (py+(sy+.5)/4)*64/size
                    # Rounded tile, with a transparent margin for every theme.
                    dx, dy = max(abs(x-32)-17, 0), max(abs(y-32)-17, 0)
                    if dx*dx+dy*dy > 9*9:
                        continue
                    color = colors[name]
                    if name in ('visual-test', 'win_test'):
                        white = 19 <= x <= 45 and 19 <= y <= 45 and (int((x-19)/13)+int((y-19)/13)) % 2 == 0
                    elif name == 'rainbow':
                        white = False
                        radius = math.hypot(x-32, y-39)
                        if y <= 39 and 13 <= radius <= 22:
                            color = [(255, 176, 131), (255, 222, 143), (148, 230, 209)][min(2, int((22-radius)/3))]
                    else:
                        white = any(distance(x, y, line) < 1.65 for line in lines[name])
                    if white:
                        color = (245, 249, 255)
                    for i in range(3): sums[i] += color[i]
                    sums[3] += 255
            alpha = (sums[3]+8)//16
            samples = sums[3]//255
            pixels.extend([*(round(sums[i]/samples) if samples else 0 for i in range(3)), alpha])
    return png(size, size, pixels)


def main():
    for size in (16, 24, 32, 48, 64, 128, 256):
        folder = ROOT / f'{size}x{size}/apps'
        folder.mkdir(parents=True, exist_ok=True)
        for name in ('terminal', 'application-default', 'visual-test', 'rainbow', 'boot-log', 'counter', 'win_test'):
            (folder / f'{name}.png').write_bytes(icon(name, size))


if __name__ == '__main__':
    main()
