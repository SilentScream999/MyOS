"""Bound actual idle hub GET_STATUS traffic with one/two eight-port hubs."""
from pathlib import Path
import subprocess
import time
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]
smoke.BOOT_ISO = True
symbols = subprocess.check_output(['nm', '-n', str(ROOT/'wyzar')], text=True)
counter = next(int(line.split()[0], 16) for line in symbols.splitlines()
               if line.split()[-1].endswith('g_usb_hub_status_requests'))

for depth in (1, 2):
    smoke.IMAGE = ROOT/f'.storage-test/hub-idle-{depth}.img'
    smoke.IMAGE.write_bytes((ROOT/'.storage-test/root.img').read_bytes())
    q = smoke.Qemu(hub_depth=depth)
    def sample():
        path = ROOT/'.storage-test/hub-idle-counter.bin'
        assert not q.monitor(f'memsave 0x{counter:x} 8 "{path}"')
        return int.from_bytes(path.read_bytes(), 'little')
    try:
        for _ in range(100):
            time.sleep(.5)
            try:
                log = q.capture(f'hub-idle-{depth}')
            except AssertionError as error:
                if 'Invalid addr' in str(error): continue
                raise
            if '[wm] Shell task started inside WM frame.' in log: break
        else: raise AssertionError('boot timeout')
        assert '[storage] WYZARDATA mounted as root /.' in log
        time.sleep(2)
        start = sample()
        time.sleep(3)
        requests = sample() - start
        assert 3*depth <= requests <= 22*depth, requests
        assert 'KERNEL PANIC' not in q.capture(f'hub-idle-{depth}-end')
        print(f'{depth} hub(s): {requests} idle GET_STATUS requests in 3 seconds '
              f'(old full-scan budget: {108*depth})')
    finally:
        q.close()
