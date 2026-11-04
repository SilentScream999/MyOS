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
2. **`SYS_WIN_GET_BUFFER` returns a kernel heap pointer** (`win->buffer`, a `kmalloc` address in the higher-half direct map). A ring-3 process should fault when it touches that. `user/win_test.c` would hit this, but it is **not in the Makefile** and nothing can launch it, so the user window API has likely never run end to end.
3. **Windows have no owner.** Any process can fetch or destroy any window id, and nothing is cleaned up when a process exits.
4. **`sys_win_create` trusts user values.** `w` and `h` are cast to `int32_t` and multiplied into a `kmalloc` size with no limits or overflow checks. The `flags` argument is never passed through (always 0), so user windows are never closable.
5. **No input for windows.** There is no event syscall and no focus model. Keyboard input reaches programs only through the TTY (fd 0); mouse input is handled entirely inside the kernel compositor.
6. **The ELF loader is minimal** (`src/exec.h`): segment `p_flags` are ignored (every page is user + writable, so no W^X); `p_vaddr`/`p_memsz` are not validated against the user address range, the stack or each other; there are no size limits and no `argv`/`envp`; page zeroing is a byte loop; pages leak on failure.
7. **No spawn / wait / exec / readdir / stat syscalls.** `execve_memory()` takes a memory buffer and is called once, at boot, for Limine module 0 (`init.elf`). `SYS_EXIT` ignores its exit code. Task cleanup frees the fd table, kernel stack and `Task`; freeing of the process's user pages and page tables was not found.
8. **Per-task state is thin:** `Task` has `pid`, `cr3`, a 16-entry `fd_table`, and entry/stack info, but no parent, exit status or owned windows.
9. **Hard-coded UI:** `wm_init` creates Terminal, Visual Test, Rainbow and Boot Log unconditionally. The Start button prints "Start Menu coming soon..." into the terminal. Terminal cannot be closed. *(Compositor special-case branches were removed in Milestone 1 — all windows now use the unified surface blit path.)*
10. **USB:** a failing device during the root-port scan can starve HID descriptor reads on other ports. Wish: detect which device is the boot drive.
11. **Repository hygiene:** build outputs (`myos`, `myos.iso` at ~12 MB, `init.elf`, `*.o`, `iso_root/`) and three ~8 MB `.raw` images are tracked, and there is no `.gitignore`. Three UUID-named `.json` files are editor project settings containing local paths. `pythonshit/` is a scratch image script.

## 6. Next steps, in order

1. ✅ ~~**Convert Terminal and Boot Log to surface-backed windows**~~ — **Done (Milestone 1 / Stage A1).** Terminal and Boot Log are now full surface clients; compositor special-case branches deleted. Tested and verified on real hardware.
2. ✅ ~~**Make the syscall path multi-process safe**~~ — **Done (Milestone 3).** Per-task kernel stack + TSS `rsp0`; user RSP preserved on private stack frame; 16-byte stack alignment enforced.
3. **Spawn/exec and app discovery per `docs/APP_LOADING.md`**, then Window API v2 (owner, mapped surfaces, events, cleanup).
4. The roadmap's later stages: hardware cursor, page flipping, GPU memory, render engine.

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
