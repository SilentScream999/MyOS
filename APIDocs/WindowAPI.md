# WyzarOS Window Management API

*Last updated: 2026-11-07.* Mapped user surfaces, owner tracking, input events and exit cleanup are implemented. The remaining limitations below still apply.

The built `win_test.elf` exercises this API from ring 3. Terminal and Start-menu
launches share a foreground session; Ctrl+C cancels the program even with its GUI
focused, closes its windows and restores terminal focus/prompt. These paths were
verified in QEMU. See [the handoff](../docs/AI_HANDOFF.md) for verification scope.

## Calling convention

`syscall` instruction: `rax` = number, arguments in `rdi, rsi, rdx, r10, r8`, result in `rax`.

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

32-bit **ARGB8888**, `buffer_pitch = w * 4` bytes, row-major. `0xFFFF0000` is solid red. **The compositor currently ignores the alpha byte:** all windows
are composited opaque. Alpha will be defined (premultiplied or straight) in roadmap stage A2.

## Lifecycle (as implemented)

1. `WIN_CREATE`: the kernel allocates the window and a zero-filled surface, appends it to the window list and schedules a redraw.
2. Draw into the surface, then call `WIN_SYNC` (everything) or `WIN_DAMAGE` (a rect). The compositor runs at about 60 Hz and copies damaged areas.
3. Clicking a user window's close button requests `WIN_EVENT_CLOSE` (type `7`, other fields zero). This request is stored separately from the input queue so mouse/key traffic cannot discard it. The next poll delivers it before queued input. Repeated clicks before delivery coalesce into one request.
4. The application handles the request: exit for a single-window app, destroy just that window, or show a save prompt. The window stays alive and visible until the app responds. `win_test` exits with status 0, waking the waiting shell and restoring its prompt without Ctrl+C.
5. `WIN_DESTROY` removes a window. Normal exit and Ctrl+C cancellation automatically destroy all windows owned by the task. Built-in system-window close buttons retain their hide behavior; Terminal cannot be closed.

Terminal, Boot Log, Visual Test and Rainbow are system windows with surfaces. All
windows use the generic compositor surface path.

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
