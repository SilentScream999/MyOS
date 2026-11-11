#!/usr/bin/env python3
"""Actual file syscalls on a grown root, plus RAMFS saves and clean poweroff."""
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from expand_usb_image import grow_image
from image_io import copy_sparse


def main():
    out = ROOT / '.storage-test'
    obj = out / 'storage-api-probe.o'
    probe = out / 'storage-api-probe.elf'
    subprocess.run(['clang', '-O1', '-ffreestanding', '-fno-stack-protector', '-fno-pic', '-fno-pie',
                    '-mno-red-zone', '-target', 'x86_64-elf', '-fno-builtin', '-c',
                    'tests/storage_api_probe.c', '-o', str(obj)], cwd=ROOT, check=True)
    subprocess.run(['ld.lld', '-nostdlib', '-z', 'max-page-size=0x1000', '-Ttext=0x400000',
                    '-o', str(probe), str(obj)], check=True)
    worker = out / 'storage-api-worker.elf'
    subprocess.run(['clang', '-O1', '-ffreestanding', '-fno-stack-protector', '-fno-pic', '-fno-pie',
                    '-mno-red-zone', '-target', 'x86_64-elf', '-fno-builtin', '-DSTORAGE_API_WORKER', '-c',
                    'tests/storage_api_probe.c', '-o', str(obj)], cwd=ROOT, check=True)
    subprocess.run(['ld.lld', '-nostdlib', '-z', 'max-page-size=0x1000', '-Ttext=0x400000',
                    '-o', str(worker), str(obj)], check=True)
    with tempfile.TemporaryDirectory(prefix='wyzar-api-') as directory:
        work = Path(directory)
        image = work / 'system.img'; volume = work / 'partition.img'
        with (ROOT / 'wyzar-usb.img').open('rb') as source, image.open('wb') as destination:
            copy_sparse(source, destination, (ROOT / 'wyzar-usb.img').stat().st_size)
        grow_image(image, 1024 * 2048)
        offset = (2048 + 262144) * 512
        with image.open('rb') as source, volume.open('wb') as destination:
            source.seek(offset); copy_sparse(source, destination, 1024 * 1024 * 1024)
        # The manifest's temporary paths contain no spaces; the executable can
        # live anywhere in the repository and is copied into this private workdir.
        local_probe = work / 'probe.elf'; local_probe.write_bytes(probe.read_bytes())
        local_worker = work / 'worker.elf'; local_worker.write_bytes(worker.read_bytes())
        manifest = work / 'manifest'; manifest.write_text(f'put /bin/apiprobe.elf {local_probe}\nput /bin/apiworker.elf {local_worker}\n')
        subprocess.run([str(out / 'ext4-image-tool'), str(volume), str(manifest)], check=True)
        with image.open('r+b') as destination, volume.open('rb') as source:
            destination.seek(offset); copy_sparse(source, destination, volume.stat().st_size)
        smoke.BOOT_ISO = True; smoke.IMAGE = image
        q = smoke.Qemu(hub_depth=2)
        powered_off = False
        try:
            log = ''
            for _ in range(90):
                time.sleep(.5)
                try:
                    log = q.capture('storage-api-start')
                except AssertionError as error:
                    if 'Invalid addr' in str(error): continue
                    raise
                if '[init] Decoupled terminal boot.' in log: break
            else:
                raise AssertionError(log[-4000:])
            assert '[storage] ext4 journal active on USB system root.' in log
            q.type('run /bin/apiprobe.elf\n', pause=.5)
            symbols = subprocess.check_output(['nm', '-C', str(ROOT / 'wyzar')], text=True).splitlines()
            foreground = next(int(line.split()[0], 16) for line in symbols if line.split()[-1] == 'g_terminal_foreground_pid')
            state = out / 'storage-api-foreground.bin'
            for _ in range(150):
                assert not q.monitor(f'memsave 0x{foreground:x} 4 "{state}"')
                if int.from_bytes(state.read_bytes(), 'little') == 0: break
                time.sleep(.2)
            else:
                raise AssertionError('Concurrent file workers did not finish')
            q.type('append /home/api-result.txt -shell\n', pause=1)
            q.type('mv /home/api-result.txt /home/api-final.txt\n', pause=1)
            q.type('shutdown\n', pause=.1)
            assert q.proc.wait(timeout=15) == 0
            powered_off = True
        finally:
            if powered_off:
                q.stream.close(); q.sock.close()
            else:
                q.capture('storage-api-failure'); q.close()
        with image.open('rb') as source, volume.open('wb') as destination:
            source.seek(offset); copy_sparse(source, destination, 1024 * 1024 * 1024)
        subprocess.run(['e2fsck', '-fn', str(volume)], check=True, capture_output=True)
        data = subprocess.check_output(['debugfs', '-R', 'cat /home/api-final.txt', str(volume)], stderr=subprocess.DEVNULL)
        assert data == b'PASS-shell', data
        for name, byte in (('even', b'E'), ('odd', b'O')):
            data = subprocess.check_output(['debugfs', '-R', f'cat /home/concurrent-{name}.bin', str(volume)], stderr=subprocess.DEVNULL)
            assert data == byte * 16385, (name, len(data))
    print('1 GiB root: actual file syscalls, RAMFS, concurrent atomic writers/exit cleanup, shell commands and clean shutdown passed')


if __name__ == '__main__':
    main()
