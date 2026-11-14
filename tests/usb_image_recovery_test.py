#!/usr/bin/env python3
"""Recovery publication/failure tests; includes real orphan recovery on Linux."""
import contextlib
import hashlib
import io
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import repair_usb_image as recovery
from ext4_volume import FEATURES, MIN_SECTORS


class RecoveryTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='recovery-test-')
        self.addCleanup(self.directory.cleanup)
        self.work = Path(self.directory.name)
        self.image = self.work / 'disk.img'
        self.output = self.work / 'disk.recovered.img'
        self.original = b'boot' * 256 + b'data' * 1024
        self.image.write_bytes(self.original)
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(patch.object(recovery, 'START', 1))
        self.stack.enter_context(patch.object(recovery, 'BOOT_SECTORS', 1))
        self.stack.enter_context(patch.object(recovery.shutil, 'which', return_value='e2fsck'))
        self.validate = self.stack.enter_context(patch.object(recovery, 'validate_existing', return_value=8))
        self.header = self.stack.enter_context(patch.object(recovery, 'check_file'))
        self.check = self.stack.enter_context(patch.object(recovery, 'check_volume', side_effect=self.repair))

    def repair(self, volume, repair):
        if repair:
            with volume.open('r+b') as destination:
                destination.write(b'fixed')

    def assert_preserved(self):
        self.assertEqual(self.image.read_bytes(), self.original)
        self.assertEqual(sorted(p.name for p in self.work.iterdir()), ['disk.img'])

    def test_separate_recovered_image(self):
        self.assertEqual(recovery.repair_image(self.image), self.output)
        self.assertEqual(self.image.read_bytes(), self.original)
        self.assertEqual(self.output.read_bytes(), self.original[:1024] + b'fixed' + self.original[1029:])
        self.assertEqual([call.kwargs['repair'] for call in self.check.call_args_list], [True, False])
        self.assertEqual(sorted(p.name for p in self.work.iterdir()), ['disk.img', 'disk.recovered.img'])

    def test_failed_repair_and_verification_do_not_publish(self):
        for failure_at in (0, 1):
            with self.subTest(failure_at=failure_at):
                self.check.side_effect = [ValueError('check failed')] if failure_at == 0 else [None, ValueError('still corrupt')]
                with self.assertRaises(ValueError):
                    recovery.repair_image(self.image)
                self.assert_preserved()

    def test_invalid_source_and_repaired_header_do_not_publish(self):
        self.validate.side_effect = ValueError('invalid layout')
        with self.assertRaises(ValueError):
            recovery.repair_image(self.image)
        self.check.assert_not_called()
        self.validate.side_effect = None
        self.header.side_effect = ValueError('invalid feature profile')
        with self.assertRaises(ValueError):
            recovery.repair_image(self.image)
        self.assert_preserved()

    def test_existing_output_and_source_alias_are_rejected(self):
        self.output.write_bytes(b'keep this')
        with self.assertRaises(FileExistsError):
            recovery.repair_image(self.image)
        self.assertEqual(self.output.read_bytes(), b'keep this')
        with self.assertRaises(ValueError):
            recovery.repair_image(self.image, self.image)
        self.check.assert_not_called()
        self.assertEqual(self.image.read_bytes(), self.original)

    def test_destination_created_during_repair_is_not_overwritten(self):
        def race(volume, repair):
            self.output.write_bytes(b'another writer')
        self.check.side_effect = race
        with self.assertRaises(FileExistsError):
            recovery.repair_image(self.image)
        self.assertEqual(self.output.read_bytes(), b'another writer')
        self.assertEqual(self.image.read_bytes(), self.original)

    def test_locked_source_is_rejected(self):
        with patch.object(recovery, 'locked_image', side_effect=RuntimeError('Close QEMU')):
            with self.assertRaisesRegex(RuntimeError, 'Close QEMU'):
                recovery.repair_image(self.image)
        self.check.assert_not_called()
        self.assert_preserved()


class CheckerTest(unittest.TestCase):
    def test_exit_codes_and_full_report(self):
        for repair in (False, True):
            for code in (0, 1, 2, 3, 4, 8, 16, 32, 128, -1):
                with self.subTest(repair=repair, code=code):
                    result = subprocess.CompletedProcess([], code, b'orphan inode\n', b'checker diagnostic\n')
                    with patch.object(recovery.subprocess, 'run', return_value=result) as run, \
                            contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                        if code in ((0, 1, 2, 3) if repair else (0,)):
                            recovery.check_volume(Path('private.img'), repair)
                        else:
                            with self.assertRaisesRegex(ValueError, 'orphan inode\nchecker diagnostic'):
                                recovery.check_volume(Path('private.img'), repair)
                        flags = ['-f', '-y'] if repair else ['-f', '-n']
                        run.assert_called_once_with(['e2fsck', *flags, 'private.img'], capture_output=True)


@unittest.skipUnless(sys.platform == 'linux' and all(shutil.which(tool) for tool in ('e2fsck', 'mke2fs', 'debugfs')),
                     'requires Linux e2fsprogs')
class RealRecoveryTest(unittest.TestCase):
    def test_orphan_reconnected_without_changing_original(self):
        with tempfile.TemporaryDirectory(prefix='real-recovery-') as directory:
            work = Path(directory)
            volume = work / 'volume.img'
            with volume.open('wb') as destination:
                destination.truncate(MIN_SECTORS * 512)
            subprocess.run(['mke2fs', '-q', '-F', '-t', 'ext4', '-b', '4096', '-I', '256',
                            '-m', '0', '-L', 'WYZARDATA', '-O', FEATURES, str(volume)], check=True, capture_output=True)
            payload = work / 'payload'
            payload.write_bytes(b'recover orphan contents')
            created = subprocess.run(['debugfs', '-w', '-R', f'write {payload} /runs.txt', str(volume)],
                                     check=True, capture_output=True)
            inode = int(re.search(rb'Allocated inode: (\d+)', created.stdout).group(1))
            subprocess.run(['debugfs', '-w', '-R', 'unlink /runs.txt', str(volume)], check=True, capture_output=True)
            start = (recovery.START + recovery.BOOT_SECTORS) * 512
            image = work / 'disk.img'
            mbr = bytearray(512)
            for i, (kind, begin, count) in enumerate(((0xef, recovery.START, recovery.BOOT_SECTORS),
                                                     (0x83, start // 512, MIN_SECTORS))):
                struct.pack_into('<B3sB3sII', mbr, 446 + i * 16, 0, b'\0'*3, kind, b'\0'*3, begin, count)
            mbr[510:512] = b'\x55\xaa'
            boot = bytearray(512)
            boot[43:54] = b'WYZARBOOT  '
            boot[510:512] = b'\x55\xaa'
            with image.open('wb') as destination:
                destination.write(mbr)
                destination.seek(recovery.START * 512)
                destination.write(boot)
                destination.seek(start)
                with volume.open('rb') as source:
                    shutil.copyfileobj(source, destination)
            def digest():
                with image.open('rb') as source:
                    return hashlib.file_digest(source, 'sha256').digest()
            before = digest()
            output = recovery.repair_image(image)
            self.assertEqual(digest(), before)
            with output.open('rb') as source, volume.open('wb') as destination:
                source.seek(start)
                shutil.copyfileobj(source, destination)
            subprocess.run(['e2fsck', '-fn', str(volume)], check=True, capture_output=True)
            contents = subprocess.run(['debugfs', '-R', f'cat /lost+found/#{inode}', str(volume)],
                                      check=True, capture_output=True)
            self.assertEqual(contents.stdout, payload.read_bytes())


if __name__ == '__main__':
    unittest.main()
