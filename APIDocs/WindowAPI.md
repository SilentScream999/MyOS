# WyzarOS Window Management API

Updated 2026-11-11. Mapped user surfaces, owner tracking, input events and exit
cleanup are implemented. The remaining authorization/mapping limitations apply.

The built `win_test.elf` exercises this API from ring 3. GUI packages declare
`type=gui` and launch independently of the shell; console apps launched from the
desktop get private terminal windows. Ctrl+C is routed by focus: terminal windows
interrupt their own program, while GUI apps receive the chord as an input event.
See [applications](../docs/APP_LOADING.md) for launch and process lifecycle.

## Calling convention

`syscall`: `rax` = number, five dispatched arguments in `rdi, rsi, rdx, r10, r8`,
result in `rax`. The sixth window-create flags argument is not dispatched.
General calls are listed in [Syscalls](Syscalls.md).

## Syscalls

| # | Name | Arguments | Returns | Notes |
|---|---|---|---|---|
| 200 | `SYS_WIN_CREATE` | `title` (max 63 chars), `x`, `y`, `w`, `h` | window id, or `0` on failure | Creates a closable user window, records owner PID and maps contiguous surface pages into the process. Dimensions are bounded to 32..3840 by 32..2160. |
| 201 | `SYS_WIN_SYNC` | `id` | `0` | Marks the **whole** surface damaged and requests a refresh. Always returns 0, even for an unknown id. |
| 202 | `SYS_WIN_DESTROY` | `id` | `0` | Removes the window from the z-stack and frees it. |
| 203 | `SYS_WIN_GET_BUFFER` | `id` | surface pointer, or `0` | User windows return their mapped user address (`USER_WIN_BASE + id * USER_WIN_STRIDE`). Window-ID authorization remains incomplete. |
| 204 | `SYS_WIN_DAMAGE` | `id`, `x`, `y`, `w`, `h` | `0`, or `-1` for an unknown id or out-of-range values | Reports a partial update (surface-relative pixels). Cheaper than `SYNC` for small changes. The compositor tracks up to 8 rects per frame. |
| 205 | `SYS_WIN_POLL_EVENT` | `id`, `WinEvent*` | `1` for an event, otherwise `0` | Returns a pending close request first, otherwise pops a nonblocking input event from the window's 64-slot queue. |

## Surface format

32-bit **ARGB8888**, `buffer_pitch = w * 4` bytes, row-major. `0xFFFF0000` is
solid red. Window surfaces are composed opaque: their alpha byte is ignored.
The desktop's PNG/font blending does not change this ABI. A future window-alpha
contract must define straight/premultiplied semantics explicitly; see the
[display roadmap](../docs/GPU_DESKTOP_ROADMAP.md).

## Lifecycle (as implemented)

1. `WIN_CREATE`: the kernel allocates/maps a surface, appends the window and
   schedules a redraw. The application initializes its pixels before presenting.
2. Draw into the surface, then call `WIN_SYNC` (everything) or `WIN_DAMAGE` (a rect). The compositor runs at about 60 Hz and copies damaged areas.
3. Clicking a user window's close button requests `WIN_EVENT_CLOSE` (type `7`, other fields zero). This request is stored separately from the input queue so mouse/key traffic cannot discard it. The next poll delivers it before queued input. Repeated clicks before delivery coalesce into one request.
4. The application handles the request: exit for a single-window app, destroy just that window, or show a save prompt. The window stays alive and visible until the app responds. `win_test` exits with status 0, waking the waiting shell and restoring its prompt without Ctrl+C.
5. `WIN_DESTROY` removes a window. Normal exit and Ctrl+C cancellation automatically destroy all windows owned by the task. Built-in system-window close buttons retain their hide behavior; Terminal cannot be closed.

Terminal, Boot Log, Visual Test and Rainbow are system windows with surfaces. All
windows use the generic compositor surface path.

Title-bar/taskbar icons are selected from app metadata by the creating process's
executable path. They load on demand through the desktop resource worker; they
are not drawn by changing the user surface's pixel format. See
[app resources](../docs/IMAGES.md).

## Known problems

* **Incomplete authorization.** Owner PID is recorded for cleanup, but window syscalls do not consistently reject another task's window ID. System-window buffer lookup can still expose a kernel address. Treat the API as trusted-prototype code.
* **User-pointer and mapping cleanup need hardening.** Event output pointers are not fully validated, and destroyed surfaces' user mappings need explicit revocation before treating this as secure isolation.
* **No present/ack protocol.** A client can write into the surface while the compositor is reading it (no double-buffering).
* **`SYNC` returns success for nonexistent windows.**

## Basic usage

```c
// Wrappers as in user/win_test.c: arguments in rdi, rsi, rdx, r10, r8.
uint64_t win = sys_win_create("My App", 100, 100, 400, 300, 0);
uint32_t* px = (uint32_t*)sys_win_get_buffer(win);      // mapped surface for this user window
for (int i = 0; i < 400 * 300; i++) px[i] = 0xFF0000FF; // opaque blue
sys_win_sync(win);                                       // or sys_win_damage(win, x, y, w, h)
```
