"""Run after make iso counter.wiz PERSIST_DISK=.storage-test/wiz-launch.img.

Only the disposable image is modified. Verify both extensions through the shell
and real Start-menu clicks, including uppercase FAT16 discovery and shell hiding.
"""
from pathlib import Path
import struct
import subprocess
import sys
import time
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from system_volume import SystemVolume
from make_usb_image import START, BOOT_SECTORS, DATA_SECTORS


def main():
    smoke.IMAGE = ROOT / '.storage-test/wiz-launch.img'
    smoke.BOOT_ISO = True
    offset = (START + BOOT_SECTORS) * 512
    with smoke.IMAGE.open('r+b') as image:
        image.seek(offset)
        volume = SystemVolume(image.read(DATA_SECTORS * 512))
        volume.put('bin/counter.wiz', (ROOT / 'counter.wiz').read_bytes())
        volume.put('bin/init.wiz', (ROOT / 'init.elf').read_bytes())
        volume.put('bin/bad.wiz', b'bad')
        volume.validate_tree()
        image.seek(offset)
        image.write(volume.data)
    symbols = subprocess.check_output(['nm', '-n', str(ROOT / 'wyzar')], text=True)
    def address(name):
        return next(int(line.split()[0], 16) for line in symbols.splitlines()
                    if line.split()[-1].endswith(name))
    q = smoke.Qemu()
    def memory(name, count):
        path = ROOT / '.storage-test/wiz-memory.bin'
        q.command('stop')
        try:
            assert not q.monitor(f'memsave 0x{address(name):x} {count} "{path}"')
            return path.read_bytes()
        finally:
            q.command('cont')
    def scalar(name, size=4, signed=False):
        return int.from_bytes(memory(name, size), 'little', signed=signed)
    def terminal():
        data = memory('g_term_matrix', 640000)[::8]
        return '\n'.join(data[i:i+160].decode('latin1').rstrip('\0 ')
                         for i in range(0, len(data), 160)).rstrip()
    def click(x, y):
        for _ in range(40):
            dx = x - scalar('g_mouse_x', signed=True)
            dy = y - scalar('g_mouse_y', signed=True)
            if abs(dx) <= 2 and abs(dy) <= 2:
                break
            q.monitor(f'mouse_move {max(-80,min(80,dx))} {max(-80,min(80,dy))}')
            time.sleep(.04)
        else:
            raise AssertionError('Mouse did not reach target')
        q.monitor('mouse_button 1'); time.sleep(.08)
        q.monitor('mouse_button 0'); time.sleep(.3)
    try:
        time.sleep(22)
        assert terminal().endswith('wyzar:/>')
        for command in ('run /bin/counter.wiz', 'run /bin/counter.elf',
                        'cd /bin', 'run counter.wiz', 'run counter.elf'):
            q.type(command + '\n', pause=3)
            text = terminal()
            assert text.endswith('wyzar:/bin>' if command != 'run /bin/counter.wiz' and command != 'run /bin/counter.elf' else 'wyzar:/>'), text
            if command.startswith('run'):
                tail = text.rsplit('Spawning:', 1)[1]
                assert 'Done! Exiting cleanly' in tail and 'Failed to spawn' not in tail, text
        q.type('run init.wiz\n', pause=.3)
        assert 'Refused:' in terminal(), terminal()
        q.type('run bad.wiz\n', pause=.3)
        assert 'Failed to spawn process' in terminal(), terminal()
        for target in ('/bin/COUNTER.WIZ', '/bin/COUNTER.ELF'):
            click(45, 748)
            assert scalar('g_menu_open', 1)
            count = scalar('g_menu_app_count')
            data = memory('g_menu_paths', 124 * count)
            paths = [p[0].split(b'\0')[0].decode() for p in struct.iter_unpack('124s', data)]
            assert '/bin/INIT.WIZ' not in paths and '/bin/INIT.ELF' not in paths, paths
            index = paths.index(target)
            hdr, row, pad = 25, 27, 7
            top = 768 - scalar('g_taskbar_h') - (hdr + count * row + pad * 2) - 2
            click(80, top + hdr + pad + index * row + row // 2)
            time.sleep(3)
            text = terminal()
            tail = text.rsplit('Spawning:', 1)[1]
            assert target in tail and 'Done! Exiting cleanly' in tail and text.endswith('wyzar:/bin>'), text
        print('PASS: .wiz/.elf absolute and relative launches, Start-menu launches, shell exclusion, invalid file rejection')
    finally:
        q.close()


if __name__ == '__main__':
    main()
