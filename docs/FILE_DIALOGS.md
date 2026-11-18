# Common file dialogs

`user/native_file_dialog.h` provides a reusable Open/Save As browser for any
native C++ app. It depends only on `native_ui.h` and the filesystem/window ABI;
it does not depend on Notepad or a running/installed Explorer process. Include
the header and declare a static `NativeFileDialog` (its bounded listing is too
large for a small stack). Packages using it must declare `abi=7` or later.
Ctrl+C and Ctrl+X copy or cut selected text in the file-name and folder-address
fields. Ctrl+V pastes text from the shared clipboard.
The compact Up and Refresh toolbar buttons use icons and the Explorer border
style. Refresh shares Start's restart artwork. Hover shows a status hint;
Alt+Up goes to the parent, Ctrl+R refreshes, and Up is dimmed at the root.
Toolbar definitions are static constexpr data so the controls also work in
native apps that enter through `_start` without running global C++ constructors.
The address field also accepts full file paths: press Enter to navigate to the
parent folder and highlight the file, then Open/Save to confirm. Relative and
double-quoted paths work, including spaces. Explicit paths can reveal files
hidden by the active filter. In Save As, a new file path selects its existing
parent folder and fills in the new filename. Missing paths leave the browser open.

Call `dialog.show(false, "/home/")` to open a file, or
`dialog.show(true, "/home/Untitled.txt")` to choose a save destination. The
optional initial path determines the starting folder and filename. A false
return means a window/surface could not be created or the window-role API is
unavailable. Render when `dialog.dirty` is true using `dialog.render()`.

Poll window events for `dialog.window` and pass them to
`dialog.event(event, frame)`, with `frame` advancing at roughly 60 Hz. It returns:

- `WAIT`: continue processing browser input.
- `ACCEPT`: `dialog.result` contains the chosen absolute path. Open mode
  requires an existing regular file; save mode also permits a new name.
- `CANCEL`: leave the app's current document unchanged.

The calling app reads or writes the chosen path, handles overwrite confirmation,
and calls `dialog.hide()` when finished. If an operation fails, keep the browser
open and call `dialog.tell("Error message")`. The picker never writes files.
Suspend document input while the picker is open. The caller owns its window,
handles events and retains its ordinary process/cleanup behavior.

The shared browser automatically sets `WIN_ROLE_FILE_DIALOG` through syscall
209. If the app currently owns keyboard focus, assigning this role raises and
focuses the dialog immediately. A background app does not steal focus.
The window manager uses the system `explorer` theme icon for its title bar
and taskbar, regardless of the calling app's executable or package icon. Ordinary
windows keep their app icon. Resources are loaded by the existing background
worker and cached; renderers do no filesystem work. Ownership and the executable
identity remain those of the caller. Syscall 209 rejects other apps' windows,
system windows and unknown roles; role 0 restores the normal app icon.

`tests/notepad_test.cpp` exercises the shared picker lifecycle and role request.
`tests/resources_test.cpp` verifies Explorer icon selection without an Explorer
executable and reuses the decoded icons without additional reads.
