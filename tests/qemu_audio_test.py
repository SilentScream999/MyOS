#!/usr/bin/env python3
"""Check analog HDA DMA through QEMU's WAV backend and no-device fallback.

Run after make iso. Boots the ISO's RAM recovery shell, without attaching or
modifying a system disk. Captured PCM proves emulator output, not laptop routing.
"""
import struct
import subprocess
import time
import wave
import qemu_storage_smoke as smoke


def boot(q, label):
    for _ in range(120):
        time.sleep(.5)
        assert q.command('query-status')['running'], label
        try:
            log = q.capture(label)
        except AssertionError:
            continue
        if '[wm] Shell task started inside WM frame.' in log:
            time.sleep(3)
            return
    raise AssertionError('Shell did not start: ' + log[-3000:])


def main():
    smoke.OUT.mkdir(exist_ok=True)
    smoke.BOOT_ISO = True
    q = smoke.Qemu(storage=False, extra_args=('-no-shutdown',))
    try:
        boot(q, 'audio-absent-boot')
        assert 'Audio: no supported analog HDA output' not in q.capture('audio-absent-idle')
        q.type('audio\n')
        log = q.capture('audio-absent')
        assert 'Audio: no supported analog HDA output' in log, log[-3000:]
        assert q.command('query-status')['running']
    finally:
        q.close()

    output = smoke.OUT / 'audio-output.wav'
    output.unlink(missing_ok=True)
    q = smoke.Qemu(storage=False, input_devices=False, extra_args=(
        '-no-shutdown', '-audiodev',
        f'wav,id=audio,path={output},out.frequency=48000,out.channels=2,out.format=s16',
        '-device', 'intel-hda', '-device', 'hda-duplex,audiodev=audio,cad=2'))
    try:
        boot(q, 'audio-hda-boot')
        # No audio command has run: background startup must initialize the
        # driver after the desktop, independently of the diagnostic helper.
        startup=q.capture('audio-hda-startup')
        assert startup.count('Audio: analog test output ready')==1,startup[-5000:]
        assert 'ThinkPad hotkeys:' not in startup,'E470 firmware setup ran on a generic guest'
        q.type('audio\n')
        log = q.capture('audio-probe')
        assert log.count('Audio: analog test output ready')==1, log[-5000:]
        assert 'Audio: codec address\n0x0000000000000002' in log, log[-5000:]
        for index in range(2):
            q.type('audio test line\n', pause=3)
            log = q.capture(f'audio-tone-{index}')
            assert log.count('Audio: final DMA position') == index + 1, log[-5000:]
            assert log.count('Audio: test DMA completed') == index + 1, log[-5000:]
            assert 'Audio: codec command timeout/error' not in log, log[-5000:]
            assert q.command('query-status')['running']
        q.type('audio test speaker\n') # unsupported route must remain usable
        q.type('audio firmware\n')
        log = q.capture('audio-firmware-unsupported')
        assert 'ThinkPad audio: firmware control unavailable on this platform' in log, log[-5000:]
        q.command('send-key', {'keys': [{'type': 'qcode', 'data': 'volumeup'}], 'hold-time': 80})
        time.sleep(1)
        log = q.capture('audio-volume-key')
        assert 'Audio: deferred multimedia make code\n0x0000000000000030' in log, log[-5000:]
        assert 'Audio: volume percent\n0x0000000000000050' in log, log[-5000:]
        q.type('audio test line alt\n') # one-DAC virtual codec must refuse safely
        log = q.capture('audio-no-alternate')
        assert 'Audio: no alternate DAC route advertised' in log, log[-5000:]
        assert log.count('Audio: final DMA position') == 2, log[-5000:]
        assert not q.monitor('sendkey ctrl-alt-t 80')
        time.sleep(2) # A second process must see the shared driver's MMIO mapping.
        q.type('audio test\n', pause=3)
        log = q.capture('audio-repeat')
        assert log.count('Audio: final DMA position') == 3, log[-5000:]
        assert log.count('Audio: test DMA completed') == 3, log[-5000:]
    finally:
        q.close()
    with wave.open(str(output), 'rb') as recording:
        assert (recording.getnchannels(), recording.getsampwidth(), recording.getframerate()) == (2, 2, 48000)
        raw = recording.readframes(recording.getnframes())
    samples = struct.unpack('<' + 'h' * (len(raw) // 2), raw)
    left, right = samples[::2], samples[1::2]
    assert left == right, 'Stereo channels differ'
    audible = [value for value in left if abs(value) > 100]
    assert len(audible) > 48000, 'Missing or truncated tone output'
    assert min(audible) < -500 and max(audible) > 500
    assert max(abs(value) for value in audible) <= 1600, 'Unexpected clipping/gain'
    # A period of 100 frames is the driver's 480 Hz tone at 48 kHz.
    start = next(i for i, value in enumerate(left) if abs(value) > 100)
    assert all(left[start + i] == left[start + i + 100] for i in range(200))
    print(f'PASS: no-device fallback, codec probe, repeated DMA, stereo 480 Hz PCM ({len(audible)} nonquiet frames)')


if __name__ == '__main__':
    main()
