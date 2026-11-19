# Notepad

Notepad is a native, resizable GUI package available in Start or through
`run /bin/notepad.wiz`. The normal initrd and persistent system-image update
include it; no fresh image is needed. Its source is `user/notepad.cpp`.

The menu bar provides File, Edit, View and Help dropdowns. File contains New,
Open, Save, Save As and Exit; Edit contains Undo, Cut, Copy, Paste, Delete and
Select All. Unavailable editing actions are dimmed. View toggles the status bar;
Help shows About. Click a menu or use Alt+F/E/V/H, arrows, Enter and Escape.
Ctrl+N, Ctrl+O, Ctrl+S and Ctrl+Shift+S remain available.
The filename and unsaved indicator share the menu row; the full path appears
in the existing status bar when space allows. Long paths shorten from the left
to keep the filename visible. The document starts directly below the menus.

Open and Save As open a separate Explorer-style file browser. Use the Home,
Applications, System and Devices sidebar, double-click folders or select one
and press Enter, and select files from the list. Up and Alt+Up navigate to the
parent folder; Ctrl+L edits the folder address. Paste a full file path there and
press Enter to reveal and select that file, then choose Open or Save. The mouse wheel scrolls the list.
Click File name to type a new save name, including spaces. All files and Text
files filters control the listing, and Tab moves between filename, list and
address fields. Lists are bounded to 512 visible entries and report truncation.
Cancel or closing the browser leaves the document intact.
Save As asks before replacing an existing file. New, Open and closing the
window ask to Save, Discard or Cancel if there are unsaved edits. A failed
read retains the current document; a failed save retains edits in memory.
Saves use VFS replacement. ext4 uses atomic replacement. External FAT32/exFAT
saves stage the new content and keep a backup during publication, with recovery
on subsequent lookup if a save is interrupted.

Click to place the caret. Arrow keys move it; Shift+arrows and Shift+click
select text. Ctrl+A selects all. Ctrl+C/X/V use the shared desktop text clipboard,
including paths copied in Explorer. Ctrl+Z undoes the last edit. The mouse wheel scrolls lines;
moving the caret scrolls horizontally or vertically to keep it visible.
Home/End move to line boundaries, Ctrl+Home/End to document boundaries, and
Delete removes the selection or next character.

This version supports ASCII text up to 64 KiB. Binary, non-ASCII and oversized
files are refused without changing them. Existing CR, LF and tab bytes survive
opening and saving; tabs and CR render as a single blank cell. New lines use LF.
Unicode, soft wrapping, search and multi-level undo are future work.

Explorer opens common text extensions (`.txt`, `.md`, `.log`, `.cfg`, `.ini`,
`.json`, `.csv`, `.c`, `.cpp`, `.h`) in Notepad on double-click. Right-click any
file and select **Open with...**, then **Notepad** to explicitly edit it.
The same menu offers Open, Copy, Cut, Rename and Delete. Directories have no
Open with handler. Other file types retain Explorer's read-only preview.
Notepad is currently the only document handler; this is not yet a persisted
association registry or an arbitrary application picker.

ABI 5 adds `OPEN_WITH` (216), taking an app path and document path, and
`DOCUMENT_PATH` (215), which copies that task's launch document into a caller
buffer and returns its length, or zero for a normal launch. Paths are copied
into each task before scheduling, so simultaneous launches do not share a
temporary file. Document handlers must be GUI packages. Existing `LAUNCH`
(213), `SPAWN` and all earlier package ABIs retain their signatures.
Mouse events now forward right/middle button transitions to native clients
as button values 2/4, alongside the existing left button value 1.

Run `make test-notepad test-explorer` in the documented WSL/Linux environment.
The tests exercise real editor/input code with mocked syscalls and Explorer
operations against a sandbox, including save/read failures, unsaved prompts,
selection, overlapping edits, limits, document handoff and resize rendering.
The editor runner writes `.storage-test/notepad-host.png` plus File menu, Open
and Save As captures for visual inspection. The common picker implementation
is in `user/native_file_dialog.h` for reuse by other native apps.
These [common dialogs](FILE_DIALOGS.md) use the Explorer icon, independent of
the app that opens them. Notepad requires ABI 7 for the shared text clipboard.
