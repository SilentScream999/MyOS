#!/usr/bin/env python3
"""Drop a codec's first ring command and verify reset + PIO probe/playback.

Uses debugger corruption only in a disposable, diskless QEMU guest. No special
driver build or production test hooks. Run after make iso; QEMU suites are serial.
"""
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor
import qemu_storage_smoke as smoke
from qemu_audio_test import boot
from qemu_debug_memory import DebugMemory


def main():
    smoke.OUT.mkdir(exist_ok=True)
    smoke.BOOT_ISO = True
    symbols = subprocess.check_output(['nm', '-n', '-C', str(smoke.ROOT / 'wyzar')], text=True).splitlines()
    def address(name):
        return next(int(line.split()[0], 16) for line in symbols if line.split(maxsplit=2)[-1] == name)
    verb = address('hda::verb(unsigned int, unsigned int, unsigned int, unsigned int&)')
    doorbell = address('hda::w16(unsigned int, unsigned short)')
    q = smoke.Qemu(storage=False, extra_args=(
        '-no-shutdown', '-gdb', 'tcp:127.0.0.1:45456',
        '-audiodev', 'none,id=audio', '-device', 'intel-hda',
        '-device', 'hda-duplex,audiodev=audio,cad=2'))
    debug = None
    try:
        boot(q, 'audio-timeout-boot')
        q.type('audio', pause=0)
        q.command('stop'); debug = DebugMemory()
        assert debug.request(f'Z0,{verb:x},1') == 'OK'
        # Resume through the debugger so its breakpoint-stop packet is consumed
        # by the matching continue request rather than a later memory operation.
        def submit():
            time.sleep(.2)
            q.type('\n', pause=0)
        with ThreadPoolExecutor(max_workers=1) as pool:
            typing = pool.submit(submit)
            assert debug.request('c').startswith(('T05', 'S05'))
            typing.result()
        assert debug.request(f'z0,{verb:x},1') == 'OK'
        assert debug.request(f'Z0,{doorbell:x},1') == 'OK'
        assert debug.request('c').startswith(('T05', 'S05'))
        # The first command has been written/flushed but not submitted. Redirect
        # its ring entry to absent codec 0; the real codec lives at address 2.
        phys = int.from_bytes(debug.read(address('hda::dma_phys'), 8), 'little')
        hhdm = int.from_bytes(debug.read(address('HHDM'), 8), 'little')
        wp = int.from_bytes(debug.read(address('hda::corb_wp'), 4), 'little')
        assert wp == 1
        debug.write(hhdm + phys + wp * 4, (0x000f0000).to_bytes(4, 'little'))
        assert debug.request(f'z0,{doorbell:x},1') == 'OK'
        debug.close(); debug = None
        time.sleep(3)
        log = q.capture('audio-timeout-recovered')
        assert 'Audio: codec command timeout/error' in log, log[-5000:]
        assert 'Audio: retrying codec with immediate commands after link reset' in log, log[-5000:]
        assert 'Audio: analog test output ready' in log, log[-5000:]
        assert 'Audio: immediate command timeout/error' not in log, log[-5000:]
        q.type('audio test line\n', pause=3)
        log = q.capture('audio-timeout-playback')
        assert 'Audio: test DMA completed' in log, log[-5000:]
        assert q.command('query-status')['running']
        print('PASS: dropped ring command, bounded timeout, link reset, PIO codec enumeration and test DMA')
    finally:
        try:
            if debug is not None:
                debug.close()
            q.capture('audio-timeout-final')
        finally:
            q.close()


if __name__ == '__main__':
    main()
