# App packages (`.wiz`)

A `.wiz` is **one file that is a complete app**: the program, its name, version,
icons and any bundled data. Copy it anywhere and it runs. Optionally install it
so it lives in the Start menu permanently.

This works the way macOS `.app` bundles, Linux AppImages and portable Windows
apps do, adapted to WyzarOS.

## The three ways to use a `.wiz`

| | What you do | Where the app lives | Start menu | Survives deleting the original |
| --- | --- | --- | --- | --- |
| **Portable** | `run /mnt/usb/notes.wiz` | wherever you put it | only if in `/bin` or `/home/Applications` | n/a |
| **Pinned** | `wiz pin /mnt/usb/notes.wiz` | wherever you put it | yes; dimmed when the file/drive is missing | no |
| **Installed** | `wiz install /mnt/usb/notes.wiz` | `/apps/notes/notes.wiz` | yes | yes |

- **Portable** needs no setup. Even a running portable app gets its own name and
  icon on its window and taskbar button.
- **Drop-in**: files in `/home/Applications` (or `/bin`) appear in Start by
  themselves. Nothing to register.
- **Pinned** records the file's location. Unplug the stick and the entry greys
  out (it remembers the name); plug it back in and it works again.
- **Installed** keeps a private, verified copy. Upgrading is `wiz install` again
  with a newer file; downgrading needs `-f`.

The `.elf` extension stays what it always was: a bare executable with no
metadata (used for the shell and system tools). Both extensions load through the
same loader, so every `.wiz` is also just an ELF.

## The `wiz` command

```text
wiz info <file|id>           name, version, contents; verifies checksums
wiz install [-f] <file.wiz>  copy into /apps/<id>/ and add to Start (-f: allow downgrade)
wiz pin <file.wiz>           add a portable package to Start without copying it
wiz remove [--purge] <id>    remove an installed or pinned app (--purge: and its data)
wiz list                     show installed and pinned apps
```

```text
wyzar:/> wiz info /share/samples/counter.wiz
Counter 1.0.0
  id          counter
  abi         1
  program     8320 bytes
  payload     11264 bytes, checksum ok
    app.manifest (163 bytes)
    icons/16.png (165 bytes)
    ...
wyzar:/> wiz install /share/samples/counter.wiz
wiz: installed Counter 1.0.0
     /apps/counter/counter.wiz
     It now appears in the Start menu.
```

Sample packages ship in `/share/samples/` (Counter, Window Test, Bundle Demo and
File Explorer). Explorer also ships in `/bin/explorer.wiz` and appears in Start.
`wiz install` verifies the whole package (every header and a CRC-32 covering the
executable and payload in format v2) before
touching `/apps`, copies to a temporary name, re-verifies the copy, and only then
swaps it in. Upgrades use an atomic file exchange: the old app remains at its
installed path until the new copy commits. A power cut or full disk cannot
expose a partial executable. Cleanup may leave a complete old copy as
`install.tmp`, which the next install removes. A first install can leave a
complete but unregistered file if power is lost before registration; rerun
`wiz install` from the original package to register it.
`remove` deletes the app but **keeps its data** in `/home/.local/share/<id>/`
unless you pass `--purge`.

## What is inside

```text
app.manifest            who the app is
icons/16.png ... 512.png  square PNGs; ship any of 16, 24, 32, 48, 64, 128, 256, 512
data/...                optional read-only resources the app can load
```

The manifest is the same `key=value` text used everywhere else:

```ini
id=notes                 # required. A-Z a-z 0-9 . _ -  (max 64). Stable identity.
name=Notes               # required. What Start and window titles show.
version=1.4.2            # required. Dotted numbers compare correctly (1.10 > 1.9).
abi=1                    # required. The WyzarOS interface level the app was built for.
description=Quick notes. # optional
icon=notes               # optional theme icon name; defaults to id
```

Unknown keys are ignored, so newer packages stay readable on older systems.
A package with a higher `abi` than the OS provides is **refused by the loader and
hidden from Start**, rather than starting and crashing.

The current OS supports ABI 3 (independent desktop launches and private console
sessions). ABI 1 and 2 packages remain supported; File Explorer requires ABI 3.

Use `type=gui` for windowed apps or `type=console` for terminal programs. An
omitted type defaults to console for legacy compatibility. Start and Explorer
open console apps in a private terminal; GUI apps open directly. Shell `run`
keeps console programs in the current terminal and returns immediately for GUI
packages. Bare `.elf` files default to console when launched from the desktop.

### Where icons come from

The first match wins:

1. A user or system theme icon with the app's icon name (`/home/.local/share/icons`,
   then `/share/icons`), so themes can re-skin any app.
2. The icon embedded in the package (nearest size: exact, else the next larger,
   else the largest smaller).
3. The generic application icon.

### Customizing an installed app

Installed apps have `/apps/<id>/app.manifest`. Add `name=` or `icon=` lines there
to override what the package says; the package fills in everything you leave out.
Upgrades keep your edits.

## Building a package

```text
make counter.wiz          # shortcut; writes build/packages/counter.wiz
make packages             # all sample packages in build/packages/
python3 tools/wiz_pack.py info build/packages/counter.wiz
```

To package a new app `foo`: put `foo.elf` in the build, create
`assets/apps/foo/app.manifest` (and optionally `assets/apps/foo/data/`), and put
icons in `assets/icons/hicolor/<N>x<N>/apps/<icon>.png`. Then
`make build/packages/foo.wiz`. Generated packages live under `build/packages/`,
which is ignored by Git and removed by `make clean`.
Without a manifest the rule falls back to a plain copy (an unpackaged `.wiz`).

`tools/wiz_pack.py` also offers `verify` and `extract`. It only accepts static
x86-64 executables, validates the manifest, checks PNG headers and sizes, and
produces byte-identical output for identical input.

## Using your own bundle from code

Apps can read their bundled files and keep their own data without knowing where
they were launched from. Include `user/wizlib.h`:

```c
#include "user/wizlib.h"

wizlib_bundle b;
if (wizlib_open(&b) == 0) {                       // 0, or WIZ_E_NOT_WIZ if run as a plain .elf
    char text[256];
    wizlib_s64 n = wizlib_read(&b, "data/greeting.txt", text, sizeof(text));
    wizlib_data_save(&b, "highscore.txt", "42", 2);   // -> /home/.local/share/<id>/highscore.txt
    wizlib_close(&b);
}
```

- The package is read-only. Save anything the app writes in its data directory,
  `/home/.local/share/<id>/` (created on demand). It survives upgrades.
- Data file names are checked: no `..`, no absolute paths.
- `user/bundle_demo.c` is a complete example (`run /share/samples/bundle_demo.wiz`).
- This uses `SYS_EXEPATH` (210), the equivalent of `/proc/self/exe`.

## Format reference (version 2)

A `.wiz` is a normal ELF followed by a payload and a footer:

```text
[ ELF image ][ ustar tar payload ][ 40-byte footer ]
```

Footer, little endian:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | `"WIZ1"` |
| 4 | 4 | format version (2) |
| 8 | 8 | `elf_size`: bytes of ELF to load |
| 16 | 8 | `payload_offset` (at least `elf_size`) |
| 24 | 8 | `payload_size` (multiple of 512, at least 1024) |
| 32 | 4 | CRC-32 of all bytes before the footer (ELF, any gap, and payload) |
| 36 | 4 | CRC-32 of footer bytes 0 to 35 |

`payload_offset + payload_size + 40` must equal the file size exactly. Because
the ELF comes first and the loader ignores trailing bytes, an unmodified loader
or any ELF tool still sees a valid executable. Format changes bump the version,
and a reader refuses versions newer than it knows. Readers also support legacy
version 1, where the checksum covers only the payload. Repack legacy apps to
gain executable corruption detection. CRC-32 detects accidental corruption;
it does not authenticate the publisher.

The payload is an ordinary ustar archive, so `tar -tf` can list it. Entries must
be regular files with relative names of letters, digits and `. _ - /`, at most 99
characters, with no `.` or `..` parts. Links, devices, pax/GNU extensions and the
ustar prefix field are rejected. If a name appears twice, the first entry wins.

### What is checked, and when

| When | Checks |
| --- | --- |
| Launch (`run`, Start) | footer, manifest present and valid, ABI supported; **only the ELF image is read into memory** |
| Start menu refresh | footer and manifest structure; icons are read lazily, one entry at a time |
| `wiz install` / `info` / `pin` | all tar headers, names, sizes and the content CRC (legacy v1: payload CRC) |

Menu-time reads skip the whole-content CRC on purpose (like a package manager
verifying at install rather than at every launch). Each PNG is still fully
validated by the decoder before use. All sizes and offsets are bounds-checked
against the file size before any read.

### Source map

| File | Role |
| --- | --- |
| `src/core/wiz.h` | the format: footer, tar walker, manifest parser (plain C, shared everywhere) |
| `src/core/exec.h` | loader: package-aware, loads only the ELF image |
| `src/gui/resources.h` | Start discovery, embedded metadata, icon fallback order, running portable apps |
| `user/wiz_cmd.h` | the `wiz` shell command |
| `user/wizlib.h` | app-side bundle and data-directory API |
| `tools/wiz_pack.py` | host packer, inspector and extractor |
| `tests/run_wiz_tests.py` | all host tests (`make test-wiz`) |
