# Project handoff

Current status, conventions, known issues and next steps, written so that anyone picking the project up cold (a new contributor, or an
AI assistant given this file as context) can get productive without the history. *Last updated: 2026-11-02.*
Read this, then `docs/GPU_DESKTOP_ROADMAP.md` and `docs/APP_LOADING.md`.

## 1. The assignment

MyOS is heading toward a **Windows-like desktop with GPU-composited rendering**: apps draw into their own surfaces, a compositor combines
them (eventually on the GPU, with alpha and shadows), the result is presented without tearing, and apps are **discovered and launched from
files** rather than being hard-coded into the kernel.

Practical constraints that shape the work:

* Development happens in WSL on Windows, and testing happens on a **real Intel Gen9 laptop**. QEMU has no Intel GPU, so GPU work cannot be checked in the emulator.
* Sessions get interrupted. **Leave the tree bootable and the docs current after every step**, and prefer small, separately testable changes.
* Keep risky experiments out of safe changes so the safe part can be committed on its own.

## 2. Current state

* The compositor is **surface-based with a damage list**: Rainbow, Visual Test and user windows draw into surfaces and report damage. Terminal and Boot Log are still special-cased.
* There is a **user window API** (syscalls 200-204), but it has never been exercised end to end (see section 5).
* The **Intel GPU layer** (blitter ring: fill, copy, fence) works on the test laptop but is **off by default** (`GPU=1` enables it) because it is slower than the CPU copy: 296 us (CPU) vs 2,911 us (GPU, no cache flush) vs 6,878 us (GPU with `clflush`) for an 800x600 frame. The benchmark is built into the kernel and runs at boot with `GPU=1`.
* Build options: `OPT` (default `-O0`) and `GPU` (default `0`); see `README.md`.

### What has been checked, and how

| Item | Evidence |
|---|---|
| Kernel builds with the default options, `GPU=1` and `OPT=-O2` | compiled and linked with clang 18 / ld.lld |
| Damage list, surfaces, rainbow row fill, GPU ring/command encoding | logic checked off-target (on a PC); not part of the repo |
| GPU blitter self-test and benchmark | run on our test laptop (boot-log output) |
| Damage list, surfaces, the HUD fixes and the other compositor changes **on screen** | **not yet booted on hardware at the time of writing** |
| `OPT=-O2` at runtime | never run |
| Tearing, flicker, drag behaviour | only judged by eye on the laptop; no measurements |

Treat "it builds" as the bar that has been met, not "it works on screen".

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

1. **The syscall path is not safe for more than one user process.** `syscall_entry` (`src/syscall.h`, around lines 345-400) saves the user stack pointer in **one global slot** (`g_saved_user_rsp`), switches to **one shared 64 KB kernel stack**, then re-enables interrupts (`sti`). With a second user task, a timer-driven task switch mid-syscall can corrupt both. It works today only because `init.elf` is the sole user process. **Fix this before launching a second user program** (per-task kernel stack, with the saved RSP in the task or via TSS `rsp0`/per-CPU data).
2. **`SYS_WIN_GET_BUFFER` returns a kernel heap pointer** (`win->buffer`, a `kmalloc` address in the higher-half direct map). A ring-3 process should fault when it touches that. `user/win_test.c` would hit this, but it is **not in the Makefile** and nothing can launch it, so the user window API has likely never run end to end.
3. **Windows have no owner.** Any process can fetch or destroy any window id, and nothing is cleaned up when a process exits.
4. **`sys_win_create` trusts user values.** `w` and `h` are cast to `int32_t` and multiplied into a `kmalloc` size with no limits or overflow checks. The `flags` argument is never passed through (always 0), so user windows are never closable.
5. **No input for windows.** There is no event syscall and no focus model. Keyboard input reaches programs only through the TTY (fd 0); mouse input is handled entirely inside the kernel compositor.
6. **The ELF loader is minimal** (`src/exec.h`): segment `p_flags` are ignored (every page is user + writable, so no W^X); `p_vaddr`/`p_memsz` are not validated against the user address range, the stack or each other; there are no size limits and no `argv`/`envp`; page zeroing is a byte loop; pages leak on failure.
7. **No spawn / wait / exec / readdir / stat syscalls.** `execve_memory()` takes a memory buffer and is called once, at boot, for Limine module 0 (`init.elf`). `SYS_EXIT` ignores its exit code. Task cleanup frees the fd table, kernel stack and `Task`; freeing of the process's user pages and page tables was not found.
8. **Per-task state is thin:** `Task` has `pid`, `cr3`, a 16-entry `fd_table`, and entry/stack info, but no parent, exit status or owned windows.
9. **Hard-coded UI:** `wm_init` creates Terminal, Visual Test, Rainbow and Boot Log unconditionally. The Start button prints "Start Menu coming soon..." into the terminal. Terminal and Boot Log are special-cased branches in the compositor rather than surfaces, and the Terminal window cannot be closed.
10. **USB:** a failing device during the root-port scan can starve HID descriptor reads on other ports. Wish: detect which device is the boot drive.
11. **Repository hygiene:** build outputs (`myos`, `myos.iso` at ~12 MB, `init.elf`, `*.o`, `iso_root/`) and three ~8 MB `.raw` images are tracked, and there is no `.gitignore`. Three UUID-named `.json` files are editor project settings containing local paths. `pythonshit/` is a scratch image script.

## 6. Next steps, in order

1. Boot the current build on the laptop. Note whether the HUD tail shows `GPU:off`, whether anything looks worse than before, and (with `GPU=1`) whether the self-test still passes.
2. Housekeeping commits: line endings, then `.gitignore` plus `git rm --cached` for build outputs, then the source changes.
3. **Convert Terminal and Boot Log to surface-backed windows using the internal API**, as Rainbow and Visual Test already are, and delete the special-case branches. Do not jump straight to making them user programs: that needs steps 4 and 5.
4. Make the syscall path multi-process safe (issue 1). Test with two cooperating user programs.
5. Spawn/exec and app discovery per `docs/APP_LOADING.md`, then Window API v2 (owner, mapped surfaces, events, cleanup).
6. The roadmap's later stages: hardware cursor, page flipping, GPU memory, render engine.

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
