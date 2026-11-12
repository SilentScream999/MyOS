"""Boot the actual USB disk behind one/two hubs; verify full reads and reboot persistence."""
from pathlib import Path
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from system_volume import SystemVolume
from make_usb_image import START, BOOT_SECTORS, DATA_SECTORS
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]
OFFSET = (START + BOOT_SECTORS) * 512

def volume(image):
    with image.open('rb') as f:
        f.seek(OFFSET)
        return SystemVolume(f.read(DATA_SECTORS * 512))

def main():
    raw = (ROOT / '.storage-test/root.img').read_bytes()
    data = SystemVolume(raw[OFFSET:OFFSET + DATA_SECTORS*512])
    data.put('bin/readhash.elf', (ROOT / '.storage-test/read-hash-probe.elf').read_bytes())
    expected = 0xcbf29ce484222325
    for byte in data.read('share/wall.raw'):
        expected = ((expected ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    symbols = subprocess.check_output(['nm','-n',str(ROOT/'wyzar')], text=True)
    pid_address = next(int(line.split()[0],16) for line in symbols.splitlines() if line.split()[-1].endswith('g_terminal_foreground_pid'))
    smoke.BOOT_ISO = False
    for depth in (1,2):
        smoke.IMAGE = ROOT / f'.storage-test/hub-{depth}.img'
        smoke.IMAGE.write_bytes(raw[:OFFSET] + data.data)
        for boot in (1,2):
            q = smoke.Qemu(hub_depth=depth)
            label = f'hub-{depth}-boot-{boot}'
            try:
                log = ''
                for _ in range(100):
                    time.sleep(.5)
                    try:
                        log = q.capture(label)
                    except AssertionError as error:
                        if 'Invalid addr' in str(error): continue
                        raise
                    if '[wm] Shell task started inside WM frame.' in log: break
                else: raise AssertionError('Hub boot timeout: ' + log[-3500:])
                assert '[storage] WYZARDATA mounted as root /.' in log, log[-3500:]
                assert '[wallpaper] Loaded disk asset into RAM.' in log, log[-3500:]
                assert 'Cleaning up non-functional hub child' not in log
                assert log.count('Hub child addressed successfully') >= depth
                if boot == 1:
                    q.type('run /bin/readhash.elf\n', pause=.2)
                    for _ in range(100):
                        path = ROOT / '.storage-test/hub-pid.bin'
                        assert not q.monitor(f'memsave 0x{pid_address:x} 4 "{path}"')
                        if int.from_bytes(path.read_bytes(),'little') == 0: break
                        time.sleep(.2)
                    else: raise AssertionError('Hub read hash probe did not finish')
                    time.sleep(.3)
                    q.type(f'write /hubtest.txt hub-depth-{depth}\n')
                else:
                    q.type('cat /hubtest.txt\n')
                q.capture(label+'-shell')
            finally: q.close()
            disk = volume(smoke.IMAGE)
            assert disk.read('hubtest.txt') == f'hub-depth-{depth}'.encode()
            assert disk.read('readhash.txt') == f'{expected:016x}'.encode(), disk.read('readhash.txt')
            print(f'Hub depth {depth}, USB cold boot {boot}: mounted, wallpaper loaded, full asset hash and persistent write verified', flush=True)

if __name__ == '__main__': main()
