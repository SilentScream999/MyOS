"""Check opt-in Boot Log performance sampling with the mock GPU backend."""
import argparse
import re
import subprocess
import time
import qemu_storage_smoke as smoke


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--quiet',action='store_true',help='Expect default logging without measurement flags')
    args=parser.parse_args()
    symbols=subprocess.check_output(['nm','-n','-C','wyzar'],text=True)
    assert 'g_gpu_test_present_calls' in symbols,'Build the mock GPU image with GPU_PRESENT_TEST first'
    smoke.BOOT_ISO=True
    q=smoke.Qemu(storage=False,resolution=(1280,800),extra_args=('-no-shutdown',))
    try:
        for _ in range(160):
            time.sleep(.25)
            try:log=q.capture('gpu-perf-boot')
            except AssertionError:continue
            if '[wm] Shell task started inside WM frame.' in log:break
        else:raise AssertionError('Shell did not boot')
        time.sleep(5)
        log=q.capture('gpu-perf-sample')
        samples=[line for line in log.splitlines() if line.startswith('GPU perf:')]
        if args.quiet:
            prefixes=('GPU perf:', 'GPU present:', 'GPU sleep:', 'GPU frame:',
                      'GPU timeline:', 'GPU Rainbow:', 'GPU launch:', 'GPU menu-',
                      'GPU workload:', 'GPU planning:')
            assert not any(line.startswith(prefixes) for line in log.splitlines()),log[-3000:]
            assert q.command('query-status')['running']
            print('Default logging omits periodic GPU reports while the desktop runs.',flush=True)
            return
        assert 1<=len(samples)<=32,(len(samples),log[-1000:])
        expected=(r'GPU perf: window=\d+ms loops=\d+ bcs=\d+ rcs=\d+ '
                  r'uploadKiB=\d+ reused=\d+ elapsed-ms scene=\d+ present=\d+ '
                  r'bcs=\d+ rcs=\d+ source=\d+ handoff=\d+ cpuAlpha=\d+ alphaKiPx=\d+ alphaMs=\d+ '
                  r'rcsAlphaKiPx=\d+ alphaSavedKiPx=\d+')
        assert all(re.fullmatch(expected,line) for line in samples),samples
        presents=[line for line in log.splitlines() if line.startswith('GPU present:')]
        sleeps=[line for line in log.splitlines() if line.startswith('GPU sleep:')]
        assert len(presents)==len(sleeps)==len(samples),(samples,presents,sleeps)
        presentation=(r'GPU present: window=(\d+)ms calls=(\d+) elapsed-ms '
                      r'copy=(\d+) overlay=(\d+) flip=(\d+) intervals=0 avgFlipMs=0 maxFlipMs=0 '
                      r'skippedRefresh=0 over2Refresh=0 cpuCopies=0')
        sleeping=(r'GPU sleep: window=(\d+)ms bcs=(\d+)/(\d+)/(\d+)/(\d+) '
                  r'rcs=(\d+)/(\d+)/(\d+)/(\d+) flip=(\d+)/(\d+)/(\d+)/(\d+)')
        for scene,present,sleep in zip(samples,presents,sleeps):
            pm=re.fullmatch(presentation,present);sm=re.fullmatch(sleeping,sleep)
            assert pm and sm,(present,sleep)
            window=int(re.search(r'window=(\d+)',scene)[1])
            assert int(pm[1])==int(sm[1])==window
            assert int(pm[2])>0 and int(pm[3])>0 and int(pm[5])>0,present
            # The mock copies pixels synchronously and never requests a GPU
            # one-ms sleep; zero is meaningful, not simulated hardware timing.
            assert all(int(v)==0 for v in sm.groups()[1:]),sleep
        assert max(map(len,samples+presents+sleeps))<512
        assert q.command('query-status')['running']
        print('GPU phase, presentation and sleep samples: matching windows, measured mock copy, zero mock GPU sleeps and bounded output passed.',flush=True)
    finally:q.close()


if __name__=='__main__':main()
