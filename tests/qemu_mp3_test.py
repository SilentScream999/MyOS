#!/usr/bin/env python3
"""Own synthesized MP3s -> user decoder -> PCM DMA -> WAV, in a diskless guest.

Requires libmp3lame.so.0 to encode fixtures, xorriso, QEMU and OVMF. The fixtures
and test ISO live under .storage-test; production images are never modified.
"""
import ctypes as C
import ctypes.util
import io
import math
import shutil
import struct
import subprocess
import tarfile
import time
import wave
import qemu_storage_smoke as smoke
from qemu_audio_test import boot


def encode(rate, channels, seconds):
    library = C.CDLL(ctypes.util.find_library('mp3lame'))
    library.lame_init.restype = C.c_void_p
    for name in ('lame_set_in_samplerate', 'lame_set_num_channels', 'lame_set_brate', 'lame_set_quality', 'lame_set_bWriteVbrTag'):
        getattr(library, name).argtypes = [C.c_void_p, C.c_int]
    library.lame_init_params.argtypes = [C.c_void_p]
    library.lame_encode_buffer_interleaved.argtypes = [C.c_void_p, C.POINTER(C.c_short), C.c_int, C.POINTER(C.c_ubyte), C.c_int]
    library.lame_encode_buffer.argtypes = [C.c_void_p, C.POINTER(C.c_short), C.POINTER(C.c_short), C.c_int, C.POINTER(C.c_ubyte), C.c_int]
    library.lame_encode_flush.argtypes = [C.c_void_p, C.POINTER(C.c_ubyte), C.c_int]
    library.lame_close.argtypes = [C.c_void_p]
    state = library.lame_init()
    assert state
    try:
        assert library.lame_set_in_samplerate(state, rate) == 0
        assert library.lame_set_num_channels(state, channels) == 0
        assert library.lame_set_brate(state, 128 if channels == 2 else 64) == 0
        assert library.lame_set_quality(state, 2) == 0
        library.lame_set_bWriteVbrTag(state, 0)
        assert library.lame_init_params(state) == 0
        frames = int(rate * seconds)
        pcm = (C.c_short * (frames * channels))()
        for i in range(frames):
            for ch in range(channels):
                pcm[i * channels + ch] = int(7000 * math.sin(2 * math.pi * (440 if not ch else 660) * i / rate))
        out = (C.c_ubyte * (frames * channels * 2 + 10000))()
        if channels == 2:
            count = library.lame_encode_buffer_interleaved(state, pcm, frames, out, len(out))
        else:
            count = library.lame_encode_buffer(state, pcm, pcm, frames, out, len(out))
        assert count > 0
        data = bytes(out[:count])
        count = library.lame_encode_flush(state, out, len(out))
        assert count >= 0
        return data + bytes(out[:count])
    finally:
        library.lame_close(state)


def fixtures():
    target = smoke.OUT / 'mp3-test.iso'
    archive = smoke.OUT / 'mp3-test-initrd.tar'
    mono = encode(22050, 1, 2)
    # Large ID3v2 tag proves metadata is skipped without buffering the whole tag.
    size = 200000
    tag = b'ID3\x04\x00\x00' + bytes((size >> 21 & 127, size >> 14 & 127, size >> 7 & 127, size & 127)) + b'\0' * size
    content = {'music.mp3': encode(44100, 2, 6), 'mono song.mp3': tag + mono,
               'short.mp3': encode(48000, 2, .04),
               'long.mp3': encode(48000, 2, 30), 'bad.mp3': b'not an MP3\n'}
    with tarfile.open(smoke.ROOT / 'initrd.tar') as original, tarfile.open(archive, 'w', format=tarfile.USTAR_FORMAT) as result:
        for entry in original:
            result.addfile(entry, original.extractfile(entry) if entry.isfile() else None)
        for name, data in content.items():
            entry = tarfile.TarInfo(name); entry.size = len(data)
            result.addfile(entry, io.BytesIO(data))
    tree = smoke.OUT / 'mp3-iso-root'
    shutil.copytree(smoke.ROOT / 'iso_root', tree, dirs_exist_ok=True)
    shutil.copyfile(archive, tree / 'boot/initrd.tar')
    subprocess.run(['xorriso', '-as', 'mkisofs', '-R', '-J', '-b', 'limine-bios-cd.bin',
                    '-no-emul-boot', '-boot-load-size', '4', '-boot-info-table',
                    '--efi-boot', 'limine-uefi-cd.bin', '-efi-boot-part', '--efi-boot-image',
                    '--protective-msdos-label', str(tree), '-o', str(target)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return target


def main():
    smoke.OUT.mkdir(exist_ok=True)
    smoke.BOOT_ISO = True; smoke.ISO_PATH = fixtures()
    recording = smoke.OUT / 'mp3-output.wav'
    recording.unlink(missing_ok=True)
    q = smoke.Qemu(storage=False, input_devices=False, extra_args=(
        '-no-shutdown', '-audiodev', f'wav,id=audio,path={recording},out.frequency=48000,out.channels=2,out.format=s16',
        '-device', 'intel-hda', '-device', 'hda-duplex,audiodev=audio,cad=2'))
    def key(code):
        keys = code.split('-')
        q.command('send-key', {'keys': [{'type': 'qcode', 'data': part} for part in keys], 'hold-time': 60})
        time.sleep(.15)
    def wait_for(text, count=1):
        for i in range(100):
            time.sleep(.3)
            log = q.capture('mp3-progress')
            if log.count(text) >= count: return log
            assert not any(text in log for text in ('Audio: PCM descriptor DMA error','Audio: PCM hardware FIFO error','Audio: PCM service deadline missed','Audio: PCM invalid DMA position','Audio: PCM underrun')), log[-4000:]
        raise AssertionError('Missing ' + text + '\n' + log[-5000:])
    try:
        boot(q, 'mp3-boot')
        q.type('play /music.mp3\n', pause=0)
        wait_for('Audio: PCM stream opened')
        time.sleep(.7); key('audiomute'); time.sleep(.7); key('audiomute')
        time.sleep(.4); key('volumedown'); key('volumedown'); key('volumeup')
        log = wait_for('Audio: PCM drain completed')
        assert 'Audio: PCM sample rate Hz\n0x000000000000AC44' in log, log[-5000:] # Native 44.1 kHz.
        assert 'Audio: PCM DMA ring bytes\n0x0000000000040000' in log, log[-5000:]
        assert 'Audio: muted\n0x0000000000000001' in log, log[-5000:]
        assert 'Audio: volume percent\n0x000000000000003C' in log, log[-5000:]
        assert 'Audio: PCM drain failed' not in log, log[-5000:]
        q.type('play /mono song.mp3\n', pause=0)
        wait_for('Audio: PCM drain completed', 2)
        q.type('play /bad.mp3\n', pause=1)
        q.type('play /missing.mp3\n', pause=1)
        q.type('play /long.mp3\n', pause=0)
        wait_for('Audio: PCM stream opened', 3)
        key('ctrl-c'); time.sleep(1)
        # A new stream must work after cancellation and release of its owner.
        q.type('play /mono song.mp3\n', pause=0)
        log = wait_for('Audio: PCM drain completed', 3)
        assert log.count('Audio: PCM stream opened') == 4, log[-5000:]
        q.type('play /short.mp3\n', pause=0)
        log = wait_for('Audio: PCM drain completed', 4)
        assert log.count('Audio: PCM stream opened') == 5, log[-5000:]
        assert 'Audio: PCM DMA stalled' not in log, log[-5000:]
        assert q.command('query-status')['running']
    finally:
        q.close()
    with wave.open(str(recording), 'rb') as wav:
        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (2,2,48000)
        raw = wav.readframes(wav.getnframes())
    values = struct.unpack('<' + 'h' * (len(raw)//2), raw)
    left, right = values[::2], values[1::2]
    assert sum(abs(v)>300 for v in left)>48000*6, 'Missing decoded audio'
    assert any(abs(a-b)>1000 for a,b in zip(left,right)), 'Stereo MP3 channels lost'
    # Verify actual frequencies after 44.1 -> 48 kHz conversion, independently
    # of playback/drain logs. Analyze a nonquiet stereo window.
    start = next(i for i in range(0,len(left)-4800,4800) if sum(v*v for v in left[i:i+4800])>4800*100000)
    def energy(samples, hz):
        return abs(sum(samples[start+i]*complex(math.cos(2*math.pi*hz*i/48000), math.sin(2*math.pi*hz*i/48000)) for i in range(4800)))
    assert energy(left,440)>energy(left,660)*5
    assert energy(right,660)>energy(right,440)*5
    first = next(i for i,v in enumerate(left) if abs(v)>1000)
    # A live mute must produce silence inside the first track, between audible
    # intervals, rather than merely setting a state flag in the kernel log.
    quiet = [sum(v*v for v in left[i:i+4800])/4800 for i in range(first+4800, min(first+5*48000,len(left)-4800),4800)]
    assert min(quiet)<1, 'Mute key did not silence recorded audio'
    print('PASS: stereo 44.1 kHz and tagged mono 22.05 kHz MP3, resampling, multimedia volume/mute, malformed/missing file, Ctrl+C and reopen, short-stream drain, recorded PCM frequencies')


if __name__ == '__main__': main()
