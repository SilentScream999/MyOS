#!/usr/bin/env python3
"""Author Wyzar's geometric PNG icon set. Optional artwork tool, never a build step.

The generated PNGs are checked-in runtime resources. Installed applications and
themes supply ordinary PNG files and never need this tool or an OS rebuild.
"""
from pathlib import Path
import math
import struct
import zlib
import argparse

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


def inside(x, y, points):
    odd = False
    for (ax, ay), (bx, by) in zip(points, points[1:] + points[:1]):
        if (ay > y) != (by > y) and x < ax + (bx-ax)*(y-ay)/(by-ay):
            odd = not odd
    return odd


def desktop_symbol(name, x, y):
    """Full-size desktop silhouettes, without a padded background tile."""
    color = None
    if name == 'explorer':
        if inside(x, y, [(4, 13), (7, 9), (25, 9), (31, 16), (57, 16), (60, 20), (59, 54), (5, 54)]):
            color = (209, 140, 35)
        if inside(x, y, [(7, 13), (24, 13), (30, 20), (56, 20), (56, 48), (7, 48)]):
            color = (255, 215, 115)
        if 11 <= x <= 55 and 22 <= y <= 36:
            color = (79, 158, 219)
        if 14 <= x <= 55 and 26 <= y <= 38:
            color = (236, 245, 253)
        if inside(x, y, [(3, 29), (61, 29), (57, 55), (7, 55)]):
            t = (y-29)/26
            color = (255-round(t*15), 205-round(t*31), 85-round(t*36))
        if inside(x, y, [(3, 29), (61, 29), (60.5, 31), (3.5, 31)]):
            color = (255, 229, 155)
        if inside(x, y, [(7, 53), (57.3, 53), (57, 55), (7, 55)]):
            color = (220, 151, 36)
    elif name == 'image_viewer':
        if 5 <= x <= 59 and 10 <= y <= 55:
            color = (210, 219, 238)
        if 8 <= x <= 56 and 13 <= y <= 51:
            color = (44, 73, 116)
        if 12 <= x <= 52 and 17 <= y <= 47:
            color = (105, 168, 210)
            if math.hypot(x-42, y-25) <= 5:
                color = (255, 222, 140)
            if inside(x, y, [(12, 43), (24, 26), (38, 47), (12, 47)]):
                color = (210, 235, 233)
            if inside(x, y, [(25, 47), (40, 32), (52, 43), (52, 47)]):
                color = (99, 160, 141)
    elif name == 'task_manager':
        if inside(x, y, [(7, 9), (58, 9), (61, 12), (61, 48), (58, 51), (7, 51), (4, 48), (4, 12)]):
            color = (38, 70, 101)
        if 26 <= x <= 39 and 48 <= y <= 56:
            color = (111, 146, 175) if x < 32 else (79, 116, 150)
        if inside(x, y, [(22, 54), (43, 54), (48, 58), (17, 58)]):
            color = (161, 187, 208) if y < 56 else (84, 121, 153)
        if inside(x, y, [(6, 7), (57, 7), (60, 10), (60, 46), (57, 49), (6, 49), (3, 46), (3, 10)]):
            color = (165, 197, 222)
        if 6 <= x <= 57 and 8 <= y <= 10:
            color = (221, 237, 248)
        if 7 <= x <= 56 and 12 <= y <= 44:
            t = (y-12)/32
            color = (19, 43-round(t*7), 62-round(t*7))
            if any(abs(x-col) < .4 for col in (16, 24, 32, 40, 48)) or any(abs(y-row) < .4 for row in (20, 28, 36)):
                color = (35, 73, 89)
            trace = [(9, 33), (17, 33), (23, 25), (28, 37), (34, 18), (39, 30), (45, 30), (50, 24), (54, 24)]
            if any(distance(x, y, (*a, *b)) < 1.05 for a, b in zip(trace, trace[1:])):
                color = (105, 235, 214)
        if 7 <= x <= 56 and 46 <= y <= 47:
            color = (111, 156, 191)
        if 51 <= x <= 54 and 46 <= y <= 47:
            color = (122, 224, 202)
    else:
        if 9 <= x <= 55 and 6 <= y <= 59:
            color = (33, 102, 163)
        if 13 <= x <= 54 and 5 <= y <= 56:
            color = (222, 235, 247)
        if 17 <= x <= 53 and 6 <= y <= 55:
            color = (248, 251, 255)
        if 13 <= x <= 54 and 5 <= y <= 16:
            color = (100, 182, 238) if y < 8 else (62, 155, 222)
        if 13 <= x <= 16 and 16 < y <= 56:
            color = (169, 211, 242)
        if 21 <= x <= 48 and any(abs(y-row) < .75 for row in (24, 31, 38, 45)):
            color = (127, 169, 197)
        for ring in (17, 25, 33, 41, 49):
            if distance(x, y, (ring, 3, ring, 11)) < 1.3:
                color = (54, 86, 112)
            if distance(x, y, (ring-.4, 3, ring-.4, 10)) < .45:
                color = (205, 225, 242)
    return color


def icon(name, size):
    colors = {'terminal': (59, 116, 195), 'application-default': (81, 116, 198),
              'visual-test': (39, 153, 151), 'rainbow': (143, 89, 191),
              'boot-log': (76, 132, 159), 'counter': (194, 126, 47), 'win_test': (39, 153, 151),
              'explorer': (91, 67, 132), 'notepad': (76, 115, 146)}
    lines = {
        'notepad': [(21, 18, 43, 18), (43, 18, 43, 46), (43, 46, 21, 46),
                    (21, 46, 21, 18), (26, 26, 38, 26), (26, 33, 38, 33), (26, 40, 34, 40)],
        'terminal': [(19, 23, 28, 31), (28, 31, 19, 39), (34, 40, 45, 40)],
        'boot-log': [(22, 21, 42, 21), (22, 29, 42, 29), (22, 37, 42, 37), (22, 45, 34, 45)],
        'counter': [(23, 21, 23, 43), (32, 21, 32, 43), (20, 28, 44, 28), (20, 36, 44, 36)],
        'application-default': [(20, 21, 44, 21), (20, 21, 20, 43), (44, 21, 44, 43),
                                (20, 43, 44, 43), (20, 27, 44, 27)],
        'explorer': [(19, 25, 27, 25), (27, 25, 30, 29), (30, 29, 45, 29),
                     (19, 25, 19, 43), (19, 43, 45, 43), (45, 29, 45, 43)],
    }
    pixels = bytearray()
    for py in range(size):
        for px in range(size):
            sums = [0, 0, 0, 0]
            for sy in range(4):
                for sx in range(4):
                    x, y = (px+(sx+.5)/4)*64/size, (py+(sy+.5)/4)*64/size
                    if name in ('explorer', 'notepad', 'task_manager', 'image_viewer'):
                        color = desktop_symbol(name, x, y)
                        if color is not None:
                            for i in range(3): sums[i] += color[i]
                            sums[3] += 255
                        continue
                    # Rounded tile, with a transparent margin for every theme.
                    dx, dy = max(abs(x-32)-17, 0), max(abs(y-32)-17, 0)
                    if dx*dx+dy*dy > 9*9:
                        continue
                    color = colors[name]
                    if name == 'explorer':
                        white = False
                        # A connected, sloping tab above the front panel.
                        if 19 <= x <= 45 and 24 <= y <= 43:
                            color = (204, 156, 77)
                        if 19 <= x <= 31 and 20 <= y < 26 and y >= 20 + max(0, x-27):
                            color = (227, 181, 100)
                        if 18 <= x <= 46 and 28 <= y <= 44:
                            color = (239, 194, 117)
                        if 19 <= x <= 45 and 28 <= y < 30:
                            color = (255, 220, 153)
                    elif name in ('visual-test', 'win_test'):
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
    names = ('terminal', 'application-default', 'visual-test', 'rainbow', 'boot-log', 'counter', 'win_test', 'explorer', 'notepad', 'task_manager', 'image_viewer')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--only', choices=names, help='Regenerate one icon family')
    args = parser.parse_args()
    for size in (16, 24, 32, 48, 64, 128, 256):
        folder = ROOT / f'{size}x{size}/apps'
        folder.mkdir(parents=True, exist_ok=True)
        for name in ((args.only,) if args.only else names):
            (folder / f'{name}.png').write_bytes(icon(name, size))


if __name__ == '__main__':
    main()
