#!/usr/bin/env python3
"""Fresh image replacement bypasses the old filesystem and survives failures."""
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import make_usb_image as builder


class FreshImageTest(unittest.TestCase):
    def test_fresh_replaces_corrupt_image_without_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / 'disk.img'
            original = b'old image with corrupt ext4 metadata'
            boot = SimpleNamespace(data=b'B' * 512)
            root = SimpleNamespace(data=b'R' * 4096)
            mbr = b'M' * 512
            image.write_bytes(original)
            with patch.object(builder, 'START', 1), \
                    patch.object(builder, 'validate_existing', side_effect=AssertionError('fresh must not validate old filesystem')), \
                    patch.object(builder, 'install_ext4_file', side_effect=AssertionError('fresh must not repair old filesystem')):
                builder.write_image(image, boot, root, mbr, fresh=True, system_root=Path(directory))
            self.assertEqual(image.read_bytes(), mbr + boot.data + root.data)
            self.assertEqual(list(Path(directory).iterdir()), [image])

    def test_failed_fresh_publication_preserves_original(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / 'disk.img'
            original = b'original disk bytes'
            image.write_bytes(original)
            with patch.object(builder, 'START', 1), patch.object(builder.os, 'replace', side_effect=OSError('publication failed')):
                with self.assertRaisesRegex(OSError, 'publication failed'):
                    builder.write_image(image, SimpleNamespace(data=b'B'*512), SimpleNamespace(data=b'R'*4096),
                                        b'M'*512, fresh=True)
            self.assertEqual(image.read_bytes(), original)
            self.assertEqual(list(Path(directory).iterdir()), [image])


if __name__ == '__main__':
    unittest.main()
