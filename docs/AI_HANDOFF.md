# Project handoff

Current status, conventions, known issues and next steps, written so that anyone picking the project up cold (a new contributor, or an
AI assistant given this file as context) can get productive without the history. *Last updated: 2026-11-04 (Milestone 1 / Stage A1 complete).*
Read this, then `docs/GPU_DESKTOP_ROADMAP.md` and `docs/APP_LOADING.md`.

## 1. The assignment

WyzarOS is heading toward a **Windows-like desktop with GPU-composited rendering**: apps draw into their own surfaces, a compositor combines
them (eventually on the GPU, with alpha and shadows), the result is presented without tearing, and apps are **discovered and launched from
files** rather than being hard-coded into the kernel.

Practical constraints that shape the work:

* Development happens in WSL on Windows, and testing happens on a **real Intel Gen9 laptop**. QEMU has no Intel GPU, so GPU work cannot be checked in the emulator.
* Sessions get interrupted. **Leave the tree bootable and the docs current after every step**, and prefer small, separately testable changes.
* Keep risky experiments out of safe changes so the safe part can be committed on its own.

## 2. Current state

* **Milestone 1 / Stage A1 ✅ COMPLETE:** Terminal and Boot Log are now surface-backed windows. The compositor's special-case branches for `WIN_TYPE_TERM` and `WIN_TYPE_LOG` have been **removed**; every window is composed through the single generic surface blit loop. Specifically:
  * `wm_create_window()` allocates `win->buffer` and sets `win->buffer_pitch = w * 4` for every window.
  * `wm_init()` creates `g_win_terminal` and assigns `g_backbuffer = (uint8_t*)g_win_terminal->buffer`.
  * `wm_compose_dirty()` — all type-specific drawing chains deleted; replaced by one unified `if (win->buffer) { ... memcpy_vram_sse_headless ... }` loop.
  * `terminal.h` — all `pitch32` uses changed from `fb->pitch / 4` to `g_term_max_cols * 8` (the surface buffer's true pitch).
  * `framebufferstuff.h` — `putp`, `fill_screen_fast`, `draw_char` updated to use surface pitch; bounds check against terminal surface size, not framebuffer size.
  * `kernel.cpp` — the now-redundant `kmalloc(g_term_buf_height * fb->pitch)` early terminal buffer alloc has been removed.
  * Tested and confirmed working on real laptop hardware.

* **Milestone 3 ✅ COMPLETE (Multi-process safe syscall path):**
  * Eliminated the single global `g_syscall_kstack` (64 KB) and single global `g_saved_user_rsp`.
  * `syscall_entry` now switches `%rsp` to the current task's private kernel stack (`next->stack_base + next->stack_size`) via `g_current_kstack_top` / `g_tss.rsp[0]`.
  * User RSP is preserved per-task on the task's own stack frame (`pushq g_user_rsp_scratch; ...; popq %rsp`).
  * Enforced strict 16-byte stack alignment (`subq $8, %rsp`) prior to `call syscall_dispatch` per the System V AMD64 ABI.
  * Preemptions and timer interrupts during a syscall now safely preserve and restore the task's private stack frame without clobbering other tasks.
  * Build verified: `make clean && make iso` → **0 errors, 12 warnings** (pre-existing volatile deprecations).

* **Milestone 4 ✅ COMPLETE (Dynamic App Loading & Process Spawning):**
  * **Ustar Archive Parser (`src/tar.h`):** Created full POSIX ustar parser. Unpacks `initrd.tar` directly into `ramfs` at boot time, populating directories and files automatically.
  * **VFS Directory Hierarchy & Enumeration (`src/vfs.h`, `src/ramfs.h`):**
    * Added `vfs_mkdir`, `vfs_mkdir_p`, `vfs_ensure_parent_dirs`, and directory enumeration `vfs_readdir` / `ramfs_readdir`.
    * Implemented directory indexing traversal in RamFS children linked-list.
  * **Dynamic Process Loading & Spawning (`src/exec.h`, `src/syscall.h`):**
    * Implemented `execve_path(const char* path)`: opens and loads any static ELF from VFS into an isolated user address space and user stack.
    * Kernel `kmain` boots `/bin/init.elf` via `execve_path("/bin/init.elf")` with module fallback.
    * Added syscalls: `SYS_READDIR` (40), `SYS_SPAWN` (59), and `SYS_SLEEP` (35).
  * **Test Program & Interactive Shell Commands (`user/counter.c`, `user/init.c`):**
    * Created `user/counter.c`: prints PID from `SYS_GETPID`, counts 5 ticks using `sys_sleep(500)` (0% CPU burn), and exits via `SYS_EXIT(0)`.
    * Extended `user/init.c` shell with `ls [path]`, `run <path>`, and `help`. Safeguarded against spawning duplicate init shells.
  * **Build System & Packaging (`Makefile`, `limine.conf`):**
    * `Makefile` compiles `user/counter.c` -> `counter.elf`, creates `initrd_root/bin/` with `init.elf`, `counter.elf`, and `wallpaper.raw`, archives into `initrd.tar`, and bundles into ISO.
    * Both `initrd.tar` and fallback modules loaded via `limine.conf`.
    * Build verified with `wsl make clean && wsl make iso` → **0 errors**.

* **Window API v2 (Surface Mapping & Lifecycles) ✅ COMPLETE:**
  * **Contiguous Physical Allocator (`src/pagingstuff.h`):** Added `alloc_phys_pages(count)` to allocate zeroed, contiguous physical 4 KiB frames for window backing surfaces.
  * **User-Space Surface Mapping (`src/wm.h`, `src/syscall.h`):** Assigned user windows a dedicated virtual address space: `USER_WIN_BASE (0x0000_6000_0000_0000) + win_id * 256 MiB`. In `sys_win_create`, the physical pages are mapped directly into the calling task's PML4 (`g_current_task->cr3`) with `PRESENT | WRITABLE | USER`. Ring 3 apps paint surfaces with zero copying.
  * **Window Ownership & Task Cleanup (`src/wm.h`, `src/syscall.h`):** Every window tracks `owner_pid`. When a user application exits or calls `sys_exit`, `wm_destroy_task_windows(pid)` automatically frees physical pages, destroys window structures, and flags the screen dirty for instant recomposition.
  * **Input Validation & Closable Windows (`src/wm.h`):** Window dimensions are strictly bounded (`32 <= w, h <= 3840, 2160`). All user windows are closable with the red 'X' button by default.
  * **Chrome & Dirty-Rect Fix (`src/wm.h`):** Added `wm_damage_window_full()` and `chrome_drawn` flag. Title bars, titles, and close buttons render immediately on window creation and first sync without needing to be dragged first.
  * **First Ring-3 GUI App (`user/win_test.c`, `Makefile`):** Built `win_test.elf`, creating a 480x320 window, painting a smooth color gradient across its mapped surface, and rendering an animated banner at ~30 FPS via `sys_sleep(33)` with 0% CPU burn. Bundled into `initrd.tar`.

* **Window Focus & Unified Input Handling (Milestone 5) ✅ COMPLETE:**
  * **Global Focus & Active Z-Order (`src/wm.h`, `src/kernel.cpp`):** Added `g_focused_win_id`. Clicking any window raises it to top of the stack and marks it active. Title bar chrome renders with an active slate-blue highlight (`0xFF2A4268` / `0xFF4A6898`) and bright white title text when focused, and subtle dark graphite (`0xFF22242B`) with dimmed text when inactive. Taskbar buttons light up with active status matching the focused window.
  * **Per-Window Event Ring Buffers (`src/wm.h`):** Defined `WinEvent` struct (`type`, `x`, `y`, `key`, `mods`, `delta`) and 64-slot ring buffer per `Window` with `wm_win_push_event()` and `wm_win_pop_event()`.
  * **`SYS_WIN_POLL_EVENT (205)` Syscall (`src/syscall.h`):** User-space GUI processes call `sys_win_poll_event(win_id, &ev)` to pop pending mouse and keyboard events safely without blocking.
  * **Focus-Directed Keyboard Routing (`src/wm.h`, `src/kernel.cpp`, `src/ps2kb.h`):**
    * When no window is focused / desktop is active, key presses are discarded (hook ready for desktop shortcuts).
    * Arrow keys, Shift+Up/Down, and Ctrl+Up/Down strictly respect focus:
      * Only when Terminal has focus does Shift+Up/Down scroll the terminal view (regular arrows navigate shell).
      * Only when Boot Log has focus does Up/Down scroll log output.
      * When a user GUI window has focus, all arrow keys and modifiers are delivered as `WIN_EVENT_KEY_DOWN` to that window without affecting Terminal or Boot Log.
    * Removed temporary debug shortcuts; system is ready for clean desktop hotkeys.
  * **Cursor Offset & Client Coordinate Fix (`src/kernel.cpp`):**
    * Fixed `ev.y = my - win->y`: because `win->y` marks the top of the client area (title bar is above at `win->y - g_title_h`), subtracting `g_title_h` an extra time was shifting click/scroll coordinates upward. Coordinates now line up precisely with the cursor tip.
  * **Universal Hover-Scrolling (`src/kernel.cpp`):** Mouse wheel scrolling is hover-based (like Windows 10/11): scrolling over Terminal scrolls terminal history; scrolling over Boot Log scrolls log output; scrolling over any user GUI window delivers `WIN_EVENT_MOUSE_SCROLL` directly to that window regardless of which window currently has focus.
  * **Interactive GUI App Update (`user/win_test.c`):** Updated `win_test.elf` to poll `SYS_WIN_POLL_EVENT`: clicking inside its client area paints stamps at `(ev.x, ev.y)`, scrolling over it dynamically shifts palette hue, and keys `R`/`G`/`B`/`Y`/`C` cycle brush colors and clear the canvas.

* The compositor is **fully surface-based** (all windows): Rainbow, Visual Test, Terminal, Boot Log and user windows all draw into `win->buffer` and report damage via `wm_surface_damage_all()`. No type-specific compositor branches remain.
* There is a **user window API** (syscalls 200-204), but it has never been exercised end to end (see section 5).
* The **Intel GPU layer** (blitter ring: fill, copy, fence) works on the test laptop but is **off by default** (`GPU=1` enables it) because it is slower than the CPU copy.
* Build options: `OPT` (default `-O0`) and `GPU` (default `0`); see `README.md`.

### What has been checked, and how

| Item | Evidence |
|---|---|
| Kernel builds with the default options, `GPU=1` and `OPT=-O2` | compiled and linked with clang 18 / ld.lld |
| Damage list, surfaces, rainbow row fill, GPU ring/command encoding | logic checked off-target (on a PC); not part of the repo |
| GPU blitter self-test and benchmark | run on our test laptop (boot-log output) |
| Damage list, surfaces, the HUD fixes and the other compositor changes **on screen** | **Tested on real hardware, hundreds of boots** (owner-confirmed). The earlier note saying it had not been booted was wrong. No recorded measurements or logs are kept in the repo. |
| `OPT=-O2` at runtime | never run |
| Tearing, flicker, drag behaviour | only judged by eye on the laptop; no measurements |

The compositor and the overall OS have been run extensively on the laptop. Items marked "never run" (for example `OPT=-O2`) are still unverified; everything else listed as working has been seen working.

## 3. Decisions to keep

1. **The GPU path stays opt-in (`GPU=1`)** until there is a render-engine path that does something the CPU is bad at (alpha blending, scaling, effects). Do not enable it by default before then.
2. **Default optimisation stays `-O0`.** Test `OPT=-O2` on its own, never mixed with another change.
3. **Keep logic separate from hardware access.** New logic (blending, damage handling, command/state encoding) goes in small headers with no kernel dependencies, like `damage.h`, `surface.h` and `gpu_cmds.h`, so it can be reviewed and checked off-target. Keep the hardware glue thin.
4. **Hardware-touching code must fail safe:** validate what it reads, fall back to the CPU path, and never loop forever on hardware state.
5. **Write down what you could not verify.** Mark hypotheses as hypotheses (the roadmap's claims log shows the style).
6. **Executable Formats:** Standard static ELF (`.elf`) remains the permanent base format for compatibility with clang/GCC/lld toolchains. A future native `.wiz` binary format (magic `"WIZ\x01"`, app title, icon, segments) will co-exist alongside ELF in `src/exec.h` as an optional lightweight container, never replacing ELF.

## 4. Rules and gotchas

* **No external `memcpy`.** The kernel does not provide one. Struct assignment can make the compiler emit a `memcpy` call and the link fails; copy field by field.
* **Exclusive vs inclusive edges.** Stage 2 of the compositor uses an exclusive `max_y`; Stage 1 and the VRAM flip use inclusive. `wm_surface_damage()` converts. Mistakes here show up as a stale 1-pixel line.
* **The cursor/drag overlay is drawn straight to VRAM after the last flip.** Any new flip path must keep that ordering, or the overlay is overwritten or flickers.
* **`make` does not track option changes.** Run `make clean` when changing `OPT` or `GPU`, or you will test a stale binary.
* **Single core.** Several places use `cli`/`sti` or assume one CPU for exclusivity. Other cores are detected and never started.
* **The GPU does not snoop CPU caches.** Anything the CPU writes for the GPU to read needs a cache flush first.
* **Blitter limits:** fill and copy only (no alpha, no scaling); see the claims log in the roadmap for how well that is sourced.
* **Line endings:** the repository is **LF**. On Windows, make sure the checkout and editor do not convert files to CRLF (otherwise most files show as modified). A `.gitattributes` containing `* text=auto eol=lf` plus `git add --renormalize .` in a separate commit fixes an existing checkout.

## 5. Known issues (found by reading the code)

These matter because the next big steps (apps, window API v2) run straight into them.

1. ✅ ~~**The syscall path is not safe for more than one user process.**~~ — **Fixed (Milestone 3).** Replaced shared global stack/RSP with per-task kernel stacks via `g_current_kstack_top` and TSS `rsp0`. User RSP is preserved on the task's private stack frame, enabling safe multitasking and preemption across multiple ring-3 tasks.
2. ✅ ~~**`SYS_WIN_GET_BUFFER` returns a kernel heap pointer.**~~ — **Fixed (Window API v2).** Window surface buffers for user processes are allocated from contiguous physical pages (`alloc_phys_pages`) and mapped into the user process's page table at `USER_WIN_BASE + win->id * USER_WIN_STRIDE` (`0x0000_6000_0000_0000...`) with `PRESENT | WRITABLE | USER`. Ring 3 apps can directly paint their surface with zero copying.
3. ✅ ~~**Windows have no owner.**~~ — **Fixed (Window API v2).** Windows record `owner_pid`. When a process calls `sys_exit` or terminates, `wm_destroy_task_windows()` automatically reclaims its physical pages, destroys its windows, and marks the screen region dirty for recomposition.
4. ✅ ~~**`sys_win_create` trusts user values.**~~ — **Fixed (Window API v2).** `w` and `h` are validated between 32 and 3840x2160. User windows are closable by default (`closable = true`).
5. ✅ ~~**Window Focus & Input Routing (Windows-like Model):**~~ — **Done (Milestone 5).** Added `g_focused_win_id`, click-to-focus/raise, distinct active/inactive title bar styling, hover-scrolling across all windows, per-window event queue ring buffer, `SYS_WIN_POLL_EVENT (205)` syscall, global key shortcut hooks (Ctrl+Shift+Esc, Ctrl+Alt+Del, Ctrl+Alt+T), and strict focus-following keyboard routing so user GUI apps receive keystrokes without terminal leakage.
6. **The ELF loader is minimal** (`src/exec.h`): segment `p_flags` are ignored (every page is user + writable, so no W^X); `p_vaddr`/`p_memsz` are not validated against the user address range, the stack or each other; there are no size limits and no `argv`/`envp`; page zeroing is a byte loop; pages leak on failure.
7. ✅ ~~**No spawn / wait / exec / readdir / stat syscalls.**~~ — **Done (Milestone 4).** Added `vfs_readdir`/`ramfs_readdir`, `vfs_mkdir_p`, `SYS_READDIR` (40), and `SYS_SPAWN` (59). Added `execve_path()` which dynamically loads static ELFs from the VFS. Apps can be spawned via `run <path>` or `sys_spawn()`.
8. **Per-task state is thin:** `Task` has `pid`, `cr3`, a 16-entry `fd_table`, and entry/stack info, but no parent, exit status or owned windows.
9. **Hard-coded UI:** `wm_init` creates Terminal, Visual Test, Rainbow and Boot Log unconditionally. The Start button prints "Start Menu coming soon..." into the terminal. Terminal cannot be closed. *(Compositor special-case branches were removed in Milestone 1 — all windows now use the unified surface blit path.)*
10. **USB:** a failing device during the root-port scan can starve HID descriptor reads on other ports. Wish: detect which device is the boot drive.
11. **Repository hygiene:** build outputs (`myos`, `myos.iso` at ~12 MB, `init.elf`, `*.o`, `iso_root/`) and three ~8 MB `.raw` images are tracked, and there is no `.gitignore`. Three UUID-named `.json` files are editor project settings containing local paths. `pythonshit/` is a scratch image script.

## 6. Next steps, in order

1. ✅ ~~**Convert Terminal and Boot Log to surface-backed windows**~~ — **Done (Milestone 1 / Stage A1).** Terminal and Boot Log are now full surface clients; compositor special-case branches deleted. Tested and verified on real hardware.
2. ✅ ~~**Make the syscall path multi-process safe**~~ — **Done (Milestone 3).** Per-task kernel stack + TSS `rsp0`; user RSP preserved on private stack frame; 16-byte stack alignment enforced.
3. ✅ ~~**Spawn/exec and app discovery per `docs/APP_LOADING.md`**~~ — **Done (Milestone 4).** ustar `initrd.tar` unpacked into ramfs at boot; `execve_path("/bin/init.elf")` for boot init; `SYS_SPAWN`/`SYS_READDIR` syscalls; `ls` and `run` shell commands; `counter.elf` test app.
4. ✅ ~~**Window API v2 (Surfaces & Lifecycle)**~~ — **Done.** User-space surface mapping (`USER_WIN_BASE + win->id * 256 MiB`), `alloc_phys_pages`, window ownership & exit cleanup, `win_test.elf` demo app verified on hardware.
5. ✅ ~~**Window Focus & Unified Input Handling (Windows-like Focus Model)**~~ — **Done (Milestone 5).** Active/focused window tracking (`g_focused_win_id`), click-to-focus and raise, per-window event queue ring buffers, `SYS_WIN_POLL_EVENT (205)` syscall, keyboard input follows focus (Terminal gets keys when focused; GUI apps receive `WIN_EVENT_KEY_DOWN` without terminal leakage), universal hover-scrolling, global hotkeys (Ctrl+Shift+Esc, Ctrl+Alt+Del, Ctrl+Alt+T).
6. **First Interactive GUI Apps & Start Menu:**
   * Interactive Paint / Drawing app (`paint.elf`).
   * Interactive Desktop Start Menu (lists `/bin/*.elf` and launches them).
7. **Filesystem on Boot Drive & `.wiz` Binary Support:**
   * USB Mass Storage (BOT) + FAT32 driver mounted at `/usb`.
   * Native `.wiz` executable loader co-existing with ELF (checks `"WIZ\x01"` magic, app icon/title metadata, segment loader).
8. The roadmap's later stages: hardware cursor, page flipping, GPU memory, render engine.

## 7. Starting a work session

1. Read this file, the roadmap and `README.md`.
2. `make clean && make iso`, then boot it.
3. For GPU or display problems, collect the boot-log lines starting with `GPU:` and, for visual problems, a slow-motion phone video. Don't theorise about tearing without that.
4. Change one thing at a time, and keep default build behaviour identical unless the change is agreed.
5. When you finish, update **Current state** and **Known issues** here. Stale docs are expensive.

## 8. Don'ts

* Don't enable the GPU path by default.
* Don't hard-code another app into the kernel as a permanent solution. Temporary test fixtures are fine if they are marked as such.
* Don't claim a hardware behaviour you have not observed; label it a hypothesis.
* Don't commit build outputs.
