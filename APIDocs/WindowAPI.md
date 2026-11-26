# WyzarOS Window Management API

Updated 2026-11-22. Mapped user surfaces, resizing, alpha, owner tracking, input events and exit
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
| 205 | `SYS_WIN_POLL_EVENT` | `id`, `WinEvent*` | `1` for an event, otherwise `0` | Returns close first, then a pending resize, then input from the window's 64-slot queue. |
| 206 | `SYS_WIN_RESIZABLE` | `id`, `min_width`, `min_height` | surface pointer, or `0` on failure | ABI 4. Owner-only opt-in to native resize events; reserves backing for desktop-sized surfaces. Minimums must fit the current desktop. Call before drawing. |
| 207 | `SYS_WIN_ACCEPT_RESIZE` | `id` | surface pointer, or `0` on failure | ABI 4. Owner-only; adopts the latest client dimensions and pitch. Redraw the whole surface afterwards. |
| 208 | `SYS_WIN_INFO` | `id`, `WindowInfo*` | `0`, or `-1` | ABI 4. Owner-only; returns the current drawing surface dimensions, pitch and user pointer. |
| 223 | `SYS_WIN_ALPHA` | owned `id`, mode | `0`, or `-1` | ABI 11. Mode 0 preserves opaque composition; mode 1 enables premultiplied source-over. Rejects other modes, system windows and another process's windows. Changing mode damages the full window. |
| 222 | `SYS_WIN_PUBLISH` | owned `id`, top source damage height or 0 for full | `0`, or `-1` | Copies a complete immutable snapshot, publishes it atomically and reports damage. Returns -1 for invalid requests or deferred publication; retry later. Unchanged immutable snapshots can reuse their staged RCS source. SYNC/DAMAGE buffers remain mutable and upload afresh. |
| 209 | `SYS_WIN_ROLE` | `id`, `role` | `0`, or `-1` | ABI 6. Owner-only; 0=ordinary app window, 1=common file dialog. File dialogs use the Explorer theme icon in the title bar and taskbar, independent of the caller. Other role values and system windows are refused. |

Native apps can reuse `user/native_file_dialog.h` for Open and Save As windows.
It sets the common file-dialog role automatically. See [common file dialogs](../docs/FILE_DIALOGS.md).

## Input keys and buttons

Mouse down/up `key` values are button bits: 1=left, 2=right, 4=middle.
Since ABI 13, mouse-move `key` also reports the currently held button bits.
Clients can cancel a drag after a release outside the window by checking this mask.
Native clients receive right/middle transitions in their client area; right
click also focuses the window without beginning a drag.
Keyboard events contain ASCII text, or arrows at `0x1041` through `0x1044`
(Up, Down, Right, Left). Home, End and Delete use `0x1100`, `0x1101`,
`0x1102` on USB and PS/2 keyboards. Other non-text keys use `0x2000` plus
the device scancode, so they cannot accidentally insert ASCII text.
Modifier bits are Ctrl=1, Shift=2, Alt=4, GUI=8. The desktop opens Start on
release of a standalone GUI key. Selecting an entry in Win+V history delivers
a Ctrl+V key-down with bit 16 set to identify a text paste. Apps with separate
file and text paste actions can use that bit to route the event to text input.

## Resizing

All windows have shared title-bar controls: minimize, maximize/restore and close.
Minimize keeps the application running and its taskbar button available; clicking
that button restores and focuses it. Clicking an active taskbar button minimizes
the window. Maximization fills the desktop above the taskbar; restoring returns
to the saved bounds. Double-clicking the title bar also toggles maximization.
Maximized windows must be restored before moving or dragging their resize edges.
The Terminal's close control is disabled; its minimize and maximize controls work.
Close continues to send event 7 to applications rather than terminating them.

Drag any window edge or corner. The outline previews its new bounds; releasing
the mouse commits the resize. Minimum sizes and desktop bounds are enforced.
Explorer, private consoles and built-in windows redraw at their new dimensions,
keeping text and icons at their normal size. Explorer wraps its toolbar; private
consoles reflow retained output. The main Terminal retains its character grid.

Existing apps remain resizable through compositor scaling of their original
surface. Pointer coordinates and damage rectangles are transformed accordingly;
their surface dimensions and pitch stay unchanged. For a responsive layout,
declare package `abi=4`, call 206 once and handle `WIN_EVENT_RESIZE` (type `8`).
Its `x` and `y` fields report the latest client width and height. Resize requests
coalesce separately from the input queue and cannot displace a close request.

On a resize event, call 207, then 208, update your layout and redraw before
calling `WIN_SYNC`. The mapped user pointer remains stable. Until acceptance,
the compositor scales the previous canvas. Queued pointer events using old
coordinates are discarded when dimensions change; keyboard input is retained.
The new pitch is `width * 4`; do not keep drawing with the original row width.

```c
struct WindowInfo {
    uint32_t width, height, pitch, reserved;
    uint64_t buffer;
}; // 24 bytes
```

`user/native_ui.h` supplies `native_enable_resize` and `native_accept_resize`
helpers. See `user/explorer.cpp` and `user/console.cpp` for complete clients.

## Surface format

32-bit **ARGB8888**, `buffer_pitch = w * 4` bytes, row-major. `0xFFFF0000` is
solid red. Window surfaces default to opaque: their alpha byte is ignored.
Opt in with `native_window_alpha(win, WIN_ALPHA_PREMULTIPLIED)` or syscall 223
with mode 1. Pixels then use premultiplied `0xAARRGGBB`: each RGB component
must be at most alpha. For example, half-opacity red is `0x80800000`, and
fully transparent black is `0x00000000`. The compositor blends source-over
onto the desktop using nearest sampling when resized. Mode 0 restores legacy
opaque copies. Draw and publish a complete image when changing pixel formats;
the mode applies to the currently published image as well as future images.
Title bars and resize-grip marks remain opaque. Pointer hit testing uses the
whole client rectangle, including transparent pixels. CPU fallback supports
the same pixel contract. Translucent scenes currently rebuild damaged row
backgrounds in stack order; the opaque retained fast paths remain available
when no translucent client is visible. Normal non-maximized windows receive a
cached shadow outside the outer frame. Shadows do not enlarge the client,
change its pixels or receive pointer events. Shadow edges and alpha scenes use
ordered composition while eligible opaque clients remain GPU blitted.
Title bars cache their text, icons and controls independently of client pixels.
Changing focus, hover, size, title or resources invalidates that cache; moving
the window reuses it. This adds no client ABI requirement.

The desktop's PNG/font blending does not change the default client ABI. See the
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
