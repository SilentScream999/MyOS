# WyzarOS Window Management API

*Last updated: 2026-11-02.* This documents what the kernel **does today**, including its problems, so nothing gets built on wrong assumptions.
The planned redesign (owner, mapped surfaces, events) is in `docs/APP_LOADING.md` section 3.6.

> **Status warning:** this API has probably **never run end to end from ring 3.** `SYS_WIN_GET_BUFFER` returns a kernel heap pointer, which a
> user process should fault on (see "Known problems"). `user/win_test.c` is not built by the Makefile and nothing can launch it.
> The built-in Rainbow and Visual Test windows use the same surfaces from inside the kernel, and that path does work.

## Calling convention

`syscall` instruction: `rax` = number, arguments in `rdi, rsi, rdx, r10, r8`, result in `rax`.

## Syscalls

| # | Name | Arguments | Returns | Notes |
|---|---|---|---|---|
| 200 | `SYS_WIN_CREATE` | `title` (user string, max 63 chars), `x`, `y`, `w`, `h` | window id, or `0` on failure | Allocates a `Window` and a `w*h*4` byte surface. A 6th `flags` value (bit 0 = closable) exists internally but the dispatcher always passes `0`, so user windows are never closable. |
| 201 | `SYS_WIN_SYNC` | `id` | `0` | Marks the **whole** surface damaged and requests a refresh. Always returns 0, even for an unknown id. |
| 202 | `SYS_WIN_DESTROY` | `id` | `0` | Removes the window from the z-stack and frees it. |
| 203 | `SYS_WIN_GET_BUFFER` | `id` | surface pointer, or `0` | Returns the kernel's pointer to the surface. **See known problems.** |
| 204 | `SYS_WIN_DAMAGE` | `id`, `x`, `y`, `w`, `h` | `0`, or `-1` for an unknown id or out-of-range values | Reports a partial update (surface-relative pixels). Cheaper than `SYNC` for small changes. The compositor tracks up to 8 rects per frame. |

## Surface format

32-bit **ARGB8888**, `buffer_pitch = w * 4` bytes, row-major. `0xFFFF0000` is solid red. **The compositor currently ignores the alpha byte:** all windows
are composited opaque. Alpha will be defined (premultiplied or straight) in roadmap stage A2.

## Lifecycle (as implemented)

1. `WIN_CREATE`: the kernel allocates the window and a zero-filled surface, appends it to the window list and schedules a redraw.
2. Draw into the surface, then call `WIN_SYNC` (everything) or `WIN_DAMAGE` (a rect). The compositor runs at about 60 Hz and copies damaged areas.
3. `WIN_DESTROY` removes the window. **Nothing destroys a process's windows when the process exits.**

System windows (Terminal, Boot Log) are created with `is_system = true` and a type. Terminal and Boot Log have **no surface** today (special-cased drawing);
Visual Test and Rainbow are system windows that *do* use surfaces.

## Known problems

* **Kernel pointer handed to user space.** `win->buffer` is a `kmalloc` address in the higher-half direct map, which is not mapped user-accessible. A ring-3 write to it should page-fault.
* **No ownership.** Any process can get a buffer for, sync, or destroy **any** window id.
* **No cleanup on exit.** A crashed or exited process leaves its windows behind.
* **Unvalidated sizes.** `w` and `h` go into `kmalloc(w*h*4)` after being cast to `int32_t`, with no limit or overflow check.
* **No input.** There is no syscall to receive mouse or keyboard events for a window; the compositor handles the mouse internally and the keyboard goes to the TTY only.
* **No present/ack protocol.** A client can write into the surface while the compositor is reading it (no double-buffering).
* **`SYNC` returns success for nonexistent windows.**

## Intended usage (do not rely on it yet)

```c
// Wrappers as in user/win_test.c: arguments in rdi, rsi, rdx, r10, r8.
uint64_t win = sys_win_create("My App", 100, 100, 400, 300, 0);
uint32_t* px = (uint32_t*)sys_win_get_buffer(win);      // NOT valid from ring 3 yet (see above)
for (int i = 0; i < 400 * 300; i++) px[i] = 0xFF0000FF; // opaque blue
sys_win_sync(win);                                       // or sys_win_damage(win, x, y, w, h)
```
