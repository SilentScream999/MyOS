# Runtime images and icons

Updated 2026-11-11. PNG/JPEG files load through VFS at runtime. App/theme
installation does not require rebuilding the OS. [APP_LOADING](APP_LOADING.md)
describes launch/registration; this guide defines the resource contract.

## Wallpapers

`/etc/desktop.cfg` contains one absolute image path, optionally ending in CR/LF:

```text
/home/background.png
```

PNG, JPG/JPEG, WRAW and legacy raw wallpapers are detected by contents, decoded
once at boot, stretched to the display and retained in RAM. PNG transparency is
composited over `#171d30`. Selection changes apply on reboot. Invalid/unreadable
files keep the procedural fallback; normal image refresh preserves existing
wallpaper/settings. The default setting remains `/share/wall.raw`.

WRAW has a 16-byte little-endian header: `WRAW`, uint32 width, uint32 height,
uint32 reserved zero, followed by BGRA pixels. Legacy headerless raw files must
match the display's pixel count. Neither format is required for PNG/JPEG loading.

## App metadata

Install `/apps/<id>/app.manifest`, for example:

```ini
name=Photo Viewer
exec=/apps/photo-viewer/viewer.wiz
icon=/apps/photo-viewer/icon.png
```

The directory name is the stable ID. `name` and an absolute, preferably canonical
`exec` path are required. `icon` accepts a PNG path or a theme icon name; it
defaults to the ID. A `.wiz` package embeds its own manifest and icons (see
[App packages](APP_PACKAGES.md)); a bare `.elf` does not.

Manifests use ASCII `key=value`, accepting blank lines, `#` comments, LF/CRLF and
surrounding whitespace. Duplicate recognized keys, invalid required values and
oversized input are rejected; unknown keys are reserved for future metadata.

| Value | Limit |
|---|---|
| Manifest | 4 KiB |
| ID | 95 bytes |
| Display name | 63 bytes |
| Executable path | 123 bytes |
| Icon path | 255 bytes |

Discovery also supports unregistered `*.elf` and `*.wiz` files in `/bin` and
`/home/Applications`, omitting `init` and suppressing entries registered by
executable path. An `.elf` icon name defaults to the basename without its
extension; a `.wiz` uses its embedded `id`/`icon`. Icon resolution order is user
theme, system theme, hicolor, the icon embedded in the package (nearest size),
then `application-default`. Embedded icons are cached under
`<package>#icons/<N>.png` and fingerprinted like files. The menu holds at most
16 apps. Windows match catalog entries by owner executable path, not title;
windows without a matching entry use the generic icon.

## Themes and size selection

`/etc/icons.cfg` holds a theme name, defaulting to `hicolor`. Invalid settings
use that default. IDs/icon/theme names accept ASCII letters, digits, `.`, `_`
and `-`, excluding `.` and `..`.

The same named icon can have several ordinary PNG files:

```text
/share/icons/hicolor/16x16/apps/photo-viewer.png
/share/icons/hicolor/32x32/apps/photo-viewer.png
/share/icons/hicolor/256x256/apps/photo-viewer.png
```

Lookup checks user assets under `/home/.local/share/icons/<theme>/`, then system
assets under `/share/icons/<theme>/`. Within each location it tries an exact size,
the smallest larger size, then the largest smaller size from
`16, 24, 32, 48, 64, 128, 256, 512`. Flat `<theme>/<name>.png` files are also accepted.

Missing/invalid named icons fall back to `hicolor`, then `application-default`
in the selected theme and `hicolor`, then a built-in placeholder. An absolute
PNG path is tried directly before the generic fallback. Not every size is
required. Resizing preserves aspect ratio and uses bilinear filtering.

System names are `terminal`, `visual-test`, `rainbow`, `boot-log` and
`application-default`, currently using the same `apps` context directory.
This native subset does not implement freedesktop `index.theme`, arbitrary
inheritance, symbolic recoloring, SVG or ICO decoding.

## Loading, caching and release

Discovery reads metadata without opening application PNGs. Opening Start asks a
worker to refresh app/theme metadata and load only displayed rows. Visible app
windows request title/taskbar icons independently of Start; system windows
request their own icons. The old complete catalog remains visible while work runs.

Closing a menu/window releases its scaled icon buffers in the next snapshot.
Visibility-only changes reuse metadata and existing pixels without disk scans.
Obsolete in-flight demand is discarded before publication. The renderer never
performs file access or decoding, and old snapshots are released only after
input/compositor readers finish.

For demanded resources, refresh reads/fingerprints PNG contents and reuses
decoded pixels if unchanged. This detects same-size edits and atomic replacement
without relying on timestamps. It performs reads when refreshing, not continuous
disk polling. Previously used sources can remain in an LRU cache capped at
32 images/8 MiB of pixels, allocated as needed. Scaled display snapshots are
additional allocations, not part of that source-cache cap.

Bundled PNGs are copied as files into the system image/recovery initrd, not
compiled into the kernel. Updates seed missing defaults and preserve replaced
PNGs. The optional artwork-generation script is not part of the build or app
installation workflow.

## Recovery memory

Limine's archive is unpacked into RAMFS so recovery works without a usable disk.
Both copies remain until the disk shell reaches its first stdin request. The
unused recovery tree then returns to the heap, preserving `/tmp`, `/run` and live
log nodes/subtrees. Archive and fallback `init.elf` module pages return to the
physical allocator. Descriptors are cleared before reuse; kernel/other-module/
reserved/bootloader-metadata pages are excluded. Reclaimed byte counts are logged.

Missing/invalid roots or a failed disk shell retain recovery apps/PNGs in RAMFS.
After successful disk boot, disk removal offlines storage until reboot rather
than switching root to RAM. Reboot loads a fresh archive. The full root policy
is in [PERSISTENT_STORAGE](PERSISTENT_STORAGE.md).

## Image API and limits

`src/gui/image.h` provides decode, resize, source-over draw and release functions.
Images use premultiplied `0xAARRGGBB`, preventing transparent resize fringes;
source-over currently targets an opaque desktop. Client window surfaces retain
their separate opaque-composition ABI.

PNG supports grayscale/palette/RGB/RGBA, transparency, 1/2/4/8/16-bit samples and
Adam7, converting to 8-bit channels. PNG framing/CRCs are validated before decode.
stb_image handles baseline/progressive JPEG subject to its format limitations.

| Resource | Encoded limit | Decoded limit |
|---|---|---|
| General PNG/JPEG | 64 MiB | 8192 pixels per dimension, 16 megapixels total |
| PNG icon | 4 MiB | 1 megapixel total, within the dimension limit above |

Scratch allocations have a per-request budget and are reclaimed on failure.
Concurrent decodes fail cleanly instead of spinning; the desktop uses one worker.
Color management, EXIF orientation, image saving and a standalone image viewer
are not implemented. [TESTING](TESTING.md) lists host/QEMU resource and recovery
checks; physical performance still needs measurement.
