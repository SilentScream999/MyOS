# Image Viewer

Image Viewer is a native userspace app installed at `/bin/image_viewer.wiz`.
Open it from Start, `run /bin/image_viewer.wiz`, or double-click a PNG/JPG/JPEG
in Explorer. Explorer's Open with menu also offers Image Viewer for any file;
press I to select it, or Enter to use the default for the selected extension.

Use Open or Ctrl+O for the shared file browser. The viewer detects PNG/JPEG
from contents, including progressive JPEG and transparent PNG. Transparent
pixels appear over a checkerboard. Invalid files, failed reads and exhausted
memory preserve the previous image.

The toolbar uses shared Explorer-style bordered icon buttons, with hover hints
in the status bar. Fit and 100% highlight the active view; image controls are
dimmed until a file is open.
With no image open, the picture icon and Open guidance are centered together
within the viewing area and follow window resizing.

| Control | Action |
|---|---|
| Fit, F or 0 | Fit the image to the current window |
| 100% or 1 | Show actual pixels |
| + / - or mouse wheel | Zoom between 10% and 800% |
| Drag the image | Pan while outside Fit mode |
| Arrow keys | Pan; leaving Fit starts at 100% |

The window is resizable. Fit follows its size; other views retain their zoom.
Drawing samples only the visible viewport, so zoom never allocates a large
scaled bitmap. Sampling is nearest-neighbor; the original decoded image remains
unchanged. The app has no editing, saving, EXIF rotation or color management.

Encoded files are limited to 8 MiB and decoded images to 4,194,304 pixels and
8192 per dimension. A reusable, coalescing 48 MiB userspace arena contains input,
decoder scratch and pixels, because the platform has no userspace heap syscall.
An image within the dimensional limits can still fail if it needs too much
scratch or cannot coexist with the current image. The arena is reserved in the
app's BSS for its lifetime; it is separate from the desktop's icon caches.
The loader marks its ELF and stack pages as process-owned. On task reap, the
kernel releases those pages (including this arena) and private user page tables.
Window-surface pages have separate compositor ownership and are retired at a
frame boundary. The arena remains allocated while the viewer is running.

`make test-image-viewer` exercises the real PNG/JPEG decoder and viewer with
ASan/UBSan: partial reads, failed loads, preserved documents, fit/zoom/pan,
repeated replacement and complete arena coalescing. `tests/qemu_image_viewer_test.py`
checks the app and Explorer integration in a disposable guest image, including
three reopen/close cycles measured against free physical RAM. Host
sanitizer tests and this QEMU suite passed on 2026-11-17; physical hardware
verification remains outstanding.
