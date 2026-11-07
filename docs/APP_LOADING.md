# How apps should be loaded (and how Windows does it)

*Last updated: 2026-11-07.* Goal: apps become **files plus metadata**, discovered and started at runtime. Near-term disk-backed app and settings work is in [NEXT_STEPS.md](NEXT_STEPS.md).

**Implementation update (2026-11-07):** the historical state below predates the
current VFS app loader and shell. `run <path>` now launches and waits for a child
with syscall 61 (`WAITPID`), restoring the prompt on exit. Parent/child identity,
retained exit status, waiter wakeup and orphan cleanup live in task/scheduler code.
See [AI_HANDOFF.md](AI_HANDOFF.md) for current implementation status. Start-menu
launches now pass a pending path to the shell and use the same foreground flow;
full terminal job control and app manifests remain future work.
Foreground input defaults to echo off. `TTYECHO(enabled)` (syscall 18) changes echo
for the single shared terminal and returns its previous state; `TTYFLUSH_INPUT`
(19) discards unread input. The shell restores its modes after the child exits.
Ctrl+C requests foreground cancellation with status 130. Owned windows and file
descriptors are closed, the parent wakes, and terminal focus returns. Cancellation
defers through in-progress syscalls to avoid abandoning kernel operations. This
currently targets one task, not an entire child-process group. A Start-menu request
wakes the registered shell's stdin with -2; syscall 20 consumes the pending path.
Busy foreground sessions reject additional menu launches.
The `/bin` tree is still RAMFS-backed. A persistent `/disk/bin` directory mounted
at `/bin` can reuse current app discovery and `execve_path`, but needs initial app
seeding, RAMFS fallback when storage is absent, and an explicit bundled-app update
policy. This echo change does not change where executables are stored.
Windows facts below were checked against the sources listed at the end; anything not sourced is marked.

## 1. How Windows starts an app like Calculator

Windows separates five layers. Hard-coding apps collapses all of them into the kernel.

**1. Identity and registration (what apps exist).** Modern Calculator is a *packaged app*: installed from an MSIX package whose
`AppxManifest.xml` declares its identity, entry point and capabilities. The identity is a *package family name* plus an app id, combined into an
**AUMID** (for Calculator: `Microsoft.WindowsCalculator_8wekyb3d8bbwe!App`). Classic desktop programs are plain `.exe` files that installers
register (shortcuts, registry entries). Either way, **the system knows about apps through data, not code.**

**2. The shell (how the user finds them).** The Start menu, taskbar and search are built from that registered data. The shell (`explorer.exe`) is itself an
ordinary user-mode program that starts programs by calling `CreateProcess`. For a packaged app, the documented route is to *activate* its identity
(`shell:AppsFolder\<AUMID>`); packaged apps can also declare an **execution alias** so they can be started by name from a terminal or the Run box.

**3. Process creation (the one thing the kernel really does).** For an `.exe`, `CreateProcess` (in kernel32) calls the native `NtCreateUserProcess`. The
kernel opens the file, creates an **image section** for it, creates the process object (address space, handle table), maps `ntdll.dll` and creates the
initial thread. For a packaged app, activation goes by AUMID through the activation manager; the step from identity to executable, and then to the
same process creation, is stated here from general knowledge rather than from a source.

**4. The user-mode loader (runtime linking).** The new thread starts in `ntdll!LdrInitializeThunk`. It initialises the loader and heap, parses the
executable's **import table**, loads each needed DLL (`kernel32`, `user32`, ...), patches the import address table, runs DLL initialisers and finally
jumps to the program's entry point.

**5. The runtime GUI API.** Programs reach the windowing system through libraries (`user32`/`gdi32`, with DWM behind them). Windows are **owned by the
creating thread/process**, input arrives as messages in the owner's queue, and when the process exits the system destroys its windows and handles.
(Ownership and cleanup semantics: general knowledge, not sourced here.)

**What to take from it:** apps are described by data; a *launcher* (the shell) reads that data and asks the kernel to create a process; the kernel's job
is just "load this image into a new address space and run it"; the GUI API ties windows to processes.

## 2. Where WyzarOS is today

| Layer | Today |
|---|---|
| App registry | **None.** Terminal, Visual Test, Rainbow and Boot Log are created unconditionally in `wm_init` (`src/wm.h`). |
| Shell/launcher | Start menu discovers `/bin/*.elf` and passes launches to the shell's foreground execution path. Ctrl+C cancels the foreground app and restores the prompt/focus. |
| Process creation | `execve_path` loads a static ELF from VFS. `SYS_SPAWN` (59), exact-child `WAITPID` (61), exit status and orphan cleanup are implemented. |
| Storage | RAMFS/initrd system files plus writable FAT16 at `/disk`; readdir/stat/unlink exist. `/bin` is still RAMFS-backed; persistent `/disk/bin` is planned. |
| Runtime linking | Not needed yet: programs are static ELFs linked at `0x400000`, using raw `syscall` wrappers (see `user/init.c`). |
| GUI API | Syscalls 200-205 provide mapped user surfaces, damage and input events; windows record owners and are cleaned up at exit/cancellation. Authorization checks need hardening. |
| Per-process state | PID/parent PID, CR3, private kernel stack, descriptors, wait/exit state and safe-boundary cancellation tracking. |

The kernel should not know what Calculator is. Two nuances:

* Some things are legitimately fixed at boot, on Windows too: the kernel, the compositor and the first user program. The rule is *"init is found at a known path; everything else is discovered."* A startup list read from a file (`/etc/startup`) is data, not hard-coding.
* The built-in demo windows (Rainbow, Visual Test) are useful **test fixtures** for the compositor. Keep them while developing, but mark them as such and don't grow the list.

## 3. Proposed design

Keep it small: static ELF programs, no dynamic linking, and a tiny shared C library for syscall and window helpers (WyzarOS's "user32/kernel32").

### 3.1 Packaging: one `initrd` module
Instead of one Limine module per program, build **one archive** (a plain `tar`/ustar file) holding `/bin`, `/apps` and assets, and pass it as a single
module. At boot, unpack it into ramfs. Adding an app means adding files to the archive; `limine.conf` never changes again. (`init.elf` and
`wallpaper.raw` can move into it.) A ustar reader is about a hundred lines.

### 3.2 Registration: an app manifest
Each app gets a directory `/apps/<id>/` containing `app.manifest` (simple `key=value` lines, so no JSON parser is needed) and its executable:

    name=Calculator
    exec=/apps/calc/calc.elf
    icon=/apps/calc/icon.raw
    window=320x480
    autostart=0

The launcher scans `/apps` at startup. This is the equivalent of an AUMID registry, and `id` is the stable identity (like the package family name).

### 3.3 Process creation: `execve_path` and spawn syscalls
* `execve_path(path)`: read the ELF through the VFS into a buffer and call the existing loader.
* New syscalls: `SYS_SPAWN(path, argv)` returning a pid, `SYS_WAIT(pid)` returning the exit code, `SYS_READDIR` and `SYS_STAT`. Record `parent` and `exit_code` in `Task`, and make `SYS_EXIT` store its code.
* **Harden the loader** at the same time: honour `p_flags` (no writable+executable pages), reject `p_vaddr`/`p_memsz` outside the user range or overlapping the stack, cap sizes, and free pages on failure and on process exit.

### 3.4 Prerequisite: multi-process safety
Fix the syscall entry path (global saved user RSP, one shared kernel stack, interrupts enabled) **before** a second user program runs. See
`docs/AI_HANDOFF.md` section 5, item 1. Also confirm the scheduler copes with several user tasks blocking in `SYS_READ` or `SYS_WAIT`.

### 3.5 Launcher (the Start menu)
A normal user program (`/apps/launcher`) or, initially, kernel code that reads `/apps/*/app.manifest` and builds the menu. Clicking an entry calls
`SYS_SPAWN(exec)`. The taskbar groups windows by **owner pid**. No app is named anywhere in kernel source.

### 3.6 Window API v2 (ties windows to processes)
* `Window.owner_pid`: only the owner can touch its window, and **all of a process's windows are destroyed when it exits.**
* **Don't return kernel pointers.** `WIN_CREATE` returns a handle; the kernel **maps the surface's pages into the client's address space** (user bit set) and returns *that* address. Validate `w`/`h` (limits, overflow).
* **Events:** a per-window queue of input events (key, mouse move/button, focus, close request, resize) read through a `SYS_WIN_POLL` or blocking wait, plus a focus model in the compositor.
* **Design surfaces knowing they will later become GPU-visible buffers** (page-aligned, contiguous or described by a page list). See `docs/GPU_DESKTOP_ROADMAP.md`.

### 3.7 A tiny user library (`libwyzar`)
Syscall wrappers, window/surface helpers, text drawing (reusing the 8x8 font) and an event-loop helper, statically linked into each app. This replaces the
copy-pasted wrappers in `init.c` and `win_test.c`. Add the user-program build rules to the Makefile (today only `init.elf` is built; `win_test.c` is not).

## 4. Converting Terminal and Boot Log

Two different steps; don't conflate them.

1. **Kernel-side surface conversion (first).** Make Terminal and Boot Log surface-backed windows using the *internal* window API, like Rainbow and Visual Test. Remove the special-case branches in the compositor. This needs nothing from the app-loading work and is low risk.
2. **Real user apps (later).** The terminal becomes `/apps/terminal`: a client of Window API v2 that renders text into its surface, receives keystrokes as events, and talks to a shell process through a pipe or pty. Boot Log becomes a small app that reads `/bootlog.txt`. System apps can be launched from `/etc/startup`. This needs 3.3-3.6 first, because today the user window API cannot work from ring 3 and nothing can start the program.

## 5. Suggested order and acceptance tests

1. Multi-process-safe syscall path. *Test:* two user programs printing interleaved without corruption.
2. `initrd` + ramfs unpack + `execve_path` + `SYS_SPAWN/WAIT/READDIR`. *Test:* a shell command such as `run /apps/hello/hello.elf` works and the exit code is returned.
3. Loader hardening. *Test:* a deliberately malformed ELF is rejected rather than mapped over kernel memory.
4. Window API v2 with ownership and mapped surfaces, with `win_test` as the first real client. *Test:* it draws, and killing it removes its window.
5. Manifest scan plus launcher/Start menu. *Test:* drop a new folder in `/apps`, rebuild the archive, and a new menu entry appears with **no kernel change**.
6. Terminal and Boot Log as apps.

## Sources checked (2026-11-02)

* Process creation path, image section, `ntdll` mapping, `LdrInitializeThunk`, import table and DLL loading: https://medium.com/@Achilles8284/the-birth-of-a-process-part-2-97c6fb9c42a2 ; https://www.chadduffey.com/windows/windbg/2026/04/20/BeforeMainCreateProcess ; https://www.cs.miami.edu/home/burt/journal/NT/processinit.html
* Packaged-app identity, AUMID, `shell:AppsFolder\Microsoft.WindowsCalculator_8wekyb3d8bbwe!App`, execution aliases: https://github.com/microsoft/WindowsAppSDK/discussions/2391 ; https://learn.microsoft.com/en-us/windows/apps/dev-tools/winapp-cli/debugging
* Activation of packaged apps by AUMID through the activation manager: https://www.nuget.org/packages/Microsoft.Testing.Extensions.PackagedApp/2.5.0
* Explorer starting programs by calling `CreateProcess`: https://medium.com/@bulmax9797/a-comprehensive-guide-to-process-creation-in-windows-using-notepad-exe-b5e403f7c49d
* Window ownership and cleanup semantics: general knowledge, not re-sourced.
