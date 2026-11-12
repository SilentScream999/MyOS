# Applications and executable loading

Updated 2026-11-11. See [IMAGES](IMAGES.md) for resources and
[Syscalls](../APIDocs/Syscalls.md) for the userspace interface.

## `.wiz` and `.elf`

Both extensions contain the same static x86-64 ELF executable built for the
WyzarOS syscall ABI. `make counter.wiz` copies `counter.elf`; renaming needs no
conversion. Windows executables and ordinary Linux ELFs are not made compatible.
A `.wiz` file does not yet embed icons or metadata.

The loader in `src/core/exec.h` reads ELF contents through VFS and creates a
private address space, user stack and ring-3 task. The path loader caps files
at 32 MiB. Boot uses the fixed shell path `/bin/init.elf`.

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

Start discovers `/apps/<id>/app.manifest`, then `/bin/*.wiz` and `/bin/*.elf`
files not registered by executable path. Extension matching is case-insensitive;
ext4 filenames remain case-sensitive. The legacy scan omits `init`. The current
menu holds at most 16 apps.

Opening Start refreshes resources in a worker. Installed apps and PNGs are
ordinary disk files; no kernel rebuild is needed to discover them. App-specific
icons belong with the app; shared theme assets live under `/share/icons`.
Windows associate icons by owner executable path, independently of title.

## Launch and lifecycle

`run <path>` accepts absolute or current-directory-relative paths. The shell
spawns the task, waits for that exact child and restores input modes/prompt on
completion. Start queues a path to the shell and uses this same foreground flow.

There is one foreground app per shared TTY. Extra menu launches while busy are
rejected. Child echo defaults off; apps can opt in. Unread child input is
discarded before shell editing resumes.

Ctrl+C cancels the foreground task with status 130, including when its window
has focus. Cancellation defers through in-progress syscalls, closes owned
windows/descriptors and wakes the parent. This is not general signals or process
groups. Window close buttons request `WIN_EVENT_CLOSE`; the app decides whether
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
  signals; a reusable userspace helper library is still needed.
- Window-ID authorization and user-pointer/mapping checks remain incomplete.
  Use trusted WyzarOS apps rather than treating this as secure isolation.
- Terminal, Boot Log and compositor demos remain built into `wm_init`.
- A versioned `.wiz` container/installer would be new formats/tools. Preserve
  current ELF-based `.wiz`/`.elf` compatibility if introduced; neither is needed
  for app-owned PNG assets.
