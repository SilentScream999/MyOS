# Applications and executable loading

Updated 2026-11-11. See [IMAGES](IMAGES.md) for resources and
[Syscalls](../APIDocs/Syscalls.md) for the userspace interface.

## `.wiz` and `.elf`

Both extensions contain a static x86-64 ELF executable built for the WyzarOS
syscall ABI. A `.elf` is a bare executable. A `.wiz` is an ELF plus an appended
package (manifest, icons, data) and a footer, so one file carries everything an
app needs and runs from anywhere; see [App packages](APP_PACKAGES.md) for the
format, the `wiz install/pin/remove` commands and the portable/installed model.
A `.wiz` without a footer (an older plain copy) still loads as an ordinary ELF.
Windows executables and ordinary Linux ELFs are not made compatible.

The loader in `src/core/exec.h` reads ELF contents through VFS and creates a
private address space, user stack and ring-3 task. For a package it checks the
footer, requires a valid manifest and a supported `abi`, and reads only the
ELF image into memory (the 32 MiB cap applies to that image, not to bundled
data). A file whose footer magic is present but invalid is refused. Boot uses
the fixed shell path `/bin/init.elf`.

## Registration and resources

An app can install a directory such as:

```text
/apps/notepad/
    app.manifest
    notepad.wiz
    icon.png
```

```ini
name=Notepad
exec=/apps/notepad/notepad.wiz
icon=/apps/notepad/icon.png
```

This is a packaging example, not an implemented Notepad app. The directory name
is the stable ID. `name` and an absolute `exec` path are required; `icon` can be
a PNG path or theme name and defaults to the ID. [IMAGES](IMAGES.md) defines the
syntax, limits, theme paths and lazy loading.

Start discovers `/apps/<id>/app.manifest` (installed or pinned apps), then
`/bin` and `/home/Applications` for `*.wiz` and `*.elf` files not registered by
executable path. A manifest whose `exec` is a `.wiz` may omit `name`, `icon` and
`description`; they come from the package. A package id appears once, so an
installed copy hides a portable duplicate. A package that needs a newer `abi` is
hidden. A pinned app whose file is missing is shown dimmed and cannot be
launched. Extension matching is case-insensitive;
ext4 filenames remain case-sensitive. The legacy scan omits `init`. The current
menu holds at most 16 apps.

Opening Start refreshes resources in a worker. Installed apps and PNGs are
ordinary disk files; no kernel rebuild is needed to discover them. App-specific
icons belong with the app; shared theme assets live under `/share/icons`.
Windows associate icons by owner executable path, independently of title.

## Launch and lifecycle

`run <path>` accepts absolute or current-directory-relative paths. Console apps
run as foreground children in the shell's TTY; GUI packages (`type=gui`) launch
independently and immediately return the prompt. Legacy files default to console.

Start uses a dedicated worker independent of the shell. Start and Explorer launch
GUI apps directly and console apps in a private terminal window. Each private
console has isolated stdin, stdout/stderr, line editing, raw/echo modes and EOF.
Its window remains after completion so output can be read; closing it cancels
remaining programs attached to that channel. The helper is `/share/system/console.elf`.
Console text uses the shell's unstretched 8x8 bitmap glyphs in 8x10 cells.

Ctrl+C in the main Terminal cancels its foreground console app with status 130.
Ctrl+C in a private console cancels that console's program. GUI apps receive the
key event themselves (Explorer uses it for Copy). Cancellation defers through
in-progress syscalls and closes owned windows/descriptors. This is not general
signals or process groups. Window close buttons request `WIN_EVENT_CLOSE`; the app decides whether
to exit, destroy one window or ask to save. See [Window API](../APIDocs/WindowAPI.md).

## Boot and recovery

Boot normally starts the disk shell after root validation. Missing/invalid
storage or a failed disk shell selects RAM recovery. Once a disk shell reaches
its first stdin request, unused recovery memory is retired while runtime RAM
mounts remain valid. [Storage](PERSISTENT_STORAGE.md) defines the boot contract.

## Remaining work

- ELF program-header bounds, segment ranges/overlap, permissions/W^X and complete
  cleanup need hardening. A file-size cap does not validate ELF structure.
- No dynamic linker, argument/environment passing, process groups or general
  signals; the small native helpers still need a general application SDK.
- Window-ID authorization and user-pointer/mapping checks remain incomplete.
  Use trusted WyzarOS apps rather than treating this as secure isolation.
- Terminal, Boot Log and compositor demos remain built into `wm_init`.
- File Explorer uses the shared native surface/syscall helpers in
  `user/native_ui.h`. General argument passing, Unicode text and a full widget
  toolkit remain future work. Package behavior is in [APP_PACKAGES](APP_PACKAGES.md).
