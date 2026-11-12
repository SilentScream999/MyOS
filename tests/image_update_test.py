#!/usr/bin/env python3
"""Exercise real boot-image updates with data preservation and lock failures."""
import hashlib
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
spec = importlib.util.spec_from_file_location('image_builder', ROOT / 'tools/make_usb_image.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


def digest(path, offset=0):
    with path.open('rb') as f:
        f.seek(offset)
        return hashlib.file_digest(f, 'sha256').hexdigest() if hasattr(hashlib, 'file_digest') else hashlib.sha256(f.read()).hexdigest()


def main():
    scratch = ROOT / '.storage-test'
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=scratch) as directory:
        output = Path(directory) / 'test.img'
        subprocess.run([sys.executable, str(ROOT / 'tools/make_usb_image.py'), '--output', str(output)], check=True)
        data_start = (builder.START + builder.BOOT_SECTORS) * 512
        # Marker in the data area stands for previously saved user data.
        with output.open('r+b') as f:
            f.seek(data_start + 40000)
            f.write(b'preserve-my-saved-data')
        expected_data = digest(output, data_start)
        with output.open('rb') as source:
            original_mbr = source.read(512)
        boot = builder.Fat16Image(builder.BOOT_SECTORS, 'WYZARBOOT', builder.START)
        boot.add('VERSION.TXT', b'updated boot content')
        builder.write_image(output, boot, None, None, update_boot=True)
        assert digest(output, data_start) == expected_data
        with output.open('rb') as f:
            f.seek(builder.START * 512)
            assert f.read(len(boot.data)) == boot.data
        expected_whole = digest(output)
        try:
            builder.write_image(output, boot, None, None)
            raise AssertionError('Existing image must be protected without --update-boot')
        except FileExistsError:
            pass
        with patch.object(builder.os, 'replace', side_effect=OSError('simulated publication failure')):
            try:
                builder.write_image(output, boot, None, None, update_boot=True)
                raise AssertionError('Publication failure must propagate')
            except OSError:
                pass
        assert digest(output) == expected_whole
        assert not list(output.parent.glob('*.tmp'))
        if sys.platform != 'win32':
            import fcntl
            with output.open('r+b') as locked:
                fcntl.lockf(locked, fcntl.LOCK_EX | fcntl.LOCK_NB)
                for flag in ('--update-boot', '--fresh'):
                    r = subprocess.run([sys.executable, str(ROOT / 'tools/make_usb_image.py'), '--output', str(output), flag], capture_output=True, text=True)
                    assert r.returncode != 0 and 'Close QEMU' in r.stderr
            assert digest(output) == expected_whole
        with output.open('r+b') as f:
            f.seek(446 + 16 + 8)
            f.write(b'bad!')
        malformed = digest(output)
        try:
            builder.write_image(output, boot, None, None, update_boot=True)
            raise AssertionError('Malformed image must be rejected')
        except ValueError:
            pass
        assert digest(output) == malformed
        assert not list(output.parent.glob('*.tmp'))
        clean = builder.Fat16Image(builder.DATA_SECTORS, 'WYZARDATA', builder.START + builder.BOOT_SECTORS)
        clean.add('README.TXT', b'fresh volume')
        builder.write_image(output, boot, clean, original_mbr, fresh=True)
        assert digest(output, data_start) == hashlib.sha256(clean.data).hexdigest()
        assert digest(output, data_start) != expected_data
        assert not list(output.parent.glob('*.tmp'))
    print('Image update tests passed: preservation, fresh reset, exclusive creation, lock/layout/failure protection')


if __name__ == '__main__':
    main()
