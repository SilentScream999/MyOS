"""Bounded-memory copying into new private sparse image files."""
import os
from pathlib import Path
import time


def supports_sparse(destination):
    path = str(Path(destination.name).resolve())
    windows_mount = len(path) > 7 and path.startswith('/mnt/') and path[5].isalpha() and path[6] == '/'
    # Native Linux images can leave zero holes. DrvFS sparse-image reads proved
    # unreliable in integration tests; write dense files on Windows storage.
    return os.name != 'nt' and not windows_mount


def copy_sparse(source, destination, count, sparse=True):
    sparse = sparse and supports_sparse(destination)
    trailing_hole = False
    while count:
        position = source.tell()
        for attempt in range(4):
            try:
                chunk = source.read(min(count, 1024 * 1024))
                break
            except OSError as error:
                if error.errno != 5 or attempt == 3:
                    raise OSError(error.errno, f'Image read failed at byte {position}: {error}') from error
                # Read retries have no write side effects. DrvFS can briefly
                # reject a newly-published sparse range; never substitute zeros.
                source.seek(position)
                time.sleep(.05)
        if not chunk:
            raise ValueError('Short image read')
        if sparse and chunk.count(0) == len(chunk):
            destination.seek(len(chunk), 1)
            trailing_hole = True
        else:
            destination.write(chunk)
            trailing_hole = False
        count -= len(chunk)
    end = destination.tell()
    # Materialize the final byte of a trailing zero range before setting EOF.
    if trailing_hole and end:
        destination.seek(end - 1)
        destination.write(b'\0')
    destination.truncate(end)
    destination.seek(end)
