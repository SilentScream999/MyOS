#!/usr/bin/env python3
"""Automated QEMU test for VFS commands and change directory system:
cd, pwd, mkdir, rmdir, df, ls, and relative path resolution.
"""
from pathlib import Path
import re
import subprocess
import time
import qemu_storage_smoke as smoke

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / '.storage-test'


def main():
    smoke.IMAGE = ROOT / 'wyzar-usb.img'
    smoke.BOOT_ISO = True

    symbols = subprocess.check_output(['nm', '-n', str(ROOT / 'wyzar')], text=True)
    address = re.search(r'^([0-9a-f]+) . _ZL13g_term_matrix$', symbols, re.M)[1]
    q = smoke.Qemu()

    def terminal(label):
        path = OUT / (label + '.matrix')
        q.command('stop')
        try:
            assert not q.monitor(f'memsave 0x{address} 640000 "{path}"')
            data = path.read_bytes()[::8]
            text = '\n'.join(data[i:i+160].decode('latin1').rstrip('\0 ') for i in range(0, len(data), 160)).rstrip()
            (OUT / (label + '.txt')).write_text(text)
            return text
        finally:
            q.command('cont')

    try:
        time.sleep(22) # Wait for desktop and shell init
        initial = terminal('vfs-initial')
        assert 'wyzar:/>' in initial, f"Expected prompt wyzar:/>, got:\n{initial}"

        # 1. Test pwd
        q.type('pwd\n', pause=.5)
        text = terminal('vfs-pwd')
        assert '/\n' in text, f"Expected '/' from pwd, got:\n{text}"

        # 2. Test df
        q.type('df\n', pause=.5)
        text = terminal('vfs-df')
        assert 'FAT16' in text and '/dev/root' in text, f"Expected FAT16 and /dev/root from df, got:\n{text}"

        # 3. Test mkdir
        q.type('mkdir mydir\n', pause=.5)
        text = terminal('vfs-mkdir')
        assert 'Directory created.' in text, f"Expected 'Directory created.', got:\n{text}"

        # 4. Test cd into relative path
        q.type('cd mydir\n', pause=.5)
        text = terminal('vfs-cd')
        assert 'wyzar:/mydir>' in text, f"Expected prompt 'wyzar:/mydir>', got:\n{text}"

        # 5. Test pwd in subdirectory
        q.type('pwd\n', pause=.5)
        text = terminal('vfs-pwd-sub')
        assert '/mydir' in text, f"Expected '/mydir' from pwd, got:\n{text}"

        # 6. Test write file with relative path
        q.type('write test.txt hello\n', pause=.5)
        text = terminal('vfs-write-sub')
        assert 'Write complete.' in text, f"Expected 'Write complete.', got:\n{text}"

        # 7. Test ls in current subdirectory
        q.type('ls\n', pause=.5)
        text = terminal('vfs-ls-sub')
        assert 'TEST.TXT' in text, f"Expected 'TEST.TXT' in ls output, got:\n{text}"

        # 8. Test cat with relative path
        q.type('cat test.txt\n', pause=.5)
        text = terminal('vfs-cat-sub')
        assert 'hello' in text, f"Expected 'hello' in cat output, got:\n{text}"

        # 9. Test cd ..
        q.type('cd ..\n', pause=.5)
        text = terminal('vfs-cd-up')
        assert text.rstrip().endswith('wyzar:/>'), f"Expected prompt 'wyzar:/>', got:\n{text}"

        # 10. Test rmdir non-empty fails
        q.type('rmdir mydir\n', pause=.5)
        text = terminal('vfs-rmdir-fail')
        assert 'Cannot remove directory' in text, f"Expected rmdir failure on non-empty dir, got:\n{text}"

        # 11. Remove file and clean up
        q.type('rm mydir/test.txt\n', pause=.5)
        text = terminal('vfs-rm-file')
        assert 'File deleted.' in text, f"Expected 'File deleted.', got:\n{text}"

        # 12. Test rmdir on now-empty dir
        q.type('rmdir mydir\n', pause=.5)
        text = terminal('vfs-rmdir-success')
        assert 'Directory removed.' in text, f"Expected 'Directory removed.', got:\n{text}"

        print("All VFS and change directory QEMU smoke tests passed!")
    finally:
        q.close()


if __name__ == '__main__':
    main()

