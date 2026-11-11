#!/usr/bin/env python3
"""Extract the actual hub enumeration functions and test without MMIO hardware."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.storage-test'


def main():
    OUT.mkdir(exist_ok=True)
    source = (ROOT / 'src/drivers/usbhelpers.h').read_text()
    start = source.index('static void configure_hub_slot(')
    end = source.index('#include "usb_root_ports.h"', start)
    (OUT / 'usb_hub_enumeration_under_test.h').write_text(source[start:end])
    root_source = (ROOT / 'src/drivers/usb_root_ports.h').read_text()
    start = root_source.index('static bool usb_root_reset(')
    end = root_source.index('#include "usb_device_tree.h"', start)
    (OUT / 'usb_root_reset_under_test.h').write_text(root_source[start:end])
    storage_source = (ROOT / 'src/drivers/usb_storage.h').read_text()
    start = storage_source.index('static USBStorageCompletion usb_storage_completions')
    end = storage_source.index('static bool usb_storage_bulk(', start)
    (OUT / 'usb_storage_wait_under_test.h').write_text(storage_source[start:end])
    start = storage_source.index('static bool usb_disk_write_sectors(')
    end = storage_source.index('static bool usb_disk_flush(', start)
    (OUT / 'usb_storage_write_under_test.h').write_text(storage_source[start:end])
    for name in ('usb_hub_protocol_test', 'usb_superspeed_hub_test', 'usb_hub_async_test',
                 'usb_hub_poll_policy_test', 'usb_device_tree_test', 'xhci_port_state_test', 'usb_root_reset_test',
                 'usb_storage_write_test', 'usb_storage_wait_test'):
        binary = OUT / name
        subprocess.run(['clang++', '-std=c++20', '-O1', '-g', '-no-pie', '-fsanitize=address,undefined',
                        '-I.', '-Isrc/drivers', str(ROOT / 'tests' / (name + '.cpp')), '-o', str(binary)], cwd=ROOT, check=True)
        subprocess.run([str(binary)], cwd=ROOT, check=True)


if __name__ == '__main__':
    main()
