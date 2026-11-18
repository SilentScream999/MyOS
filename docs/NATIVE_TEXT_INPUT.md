# Native text input

Include `user/native_text.h` for the common insertion caret and single-line
editing component. Notepad, Explorer and the common file dialog use these.

`NativeTextCursor` owns the shared one-pixel caret and blink timing. In a native
app's 16 ms loop, call `cursor.animate(surface, window, frame, active, dirty)`
before the normal repaint and set `dirty` if it returns true. Call
`cursor.reset(frame)` on input or a caret move and mark the view dirty, then
`cursor.draw(surface, x, y, height)` after drawing text during that repaint.
The caret saves its underlying pixel column (up to 64 pixels tall), so idle
blinking changes and damages only that column through syscall 204. Pending
repaints invalidate the saved column; larger carets fall back to a normal
repaint. Inactive fields hide their caret. Notepad keeps its document selection
and multiline navigation and uses this same caret renderer.

`NativeTextInput` keeps the caret and selection anchor for an ASCII field.
Ctrl+Left/Right moves by word, Shift extends the selection, and Ctrl+A selects
the entire field. Typing, deleting, or pasting replaces the selected range.
Clicking positions the caret at the nearest character boundary. Shift+click
extends the selection in the active field.
Notepad uses the same word boundaries in its multiline editor.

Ctrl+C, Ctrl+X and Ctrl+V share text between Notepad, Explorer's editable
fields, and the Open and Save dialogs. Win+V opens clipboard history from
anywhere on the desktop. Click an entry or use Up/Down and Enter to make it
current and paste it into the focused input. Escape dismisses the panel.
The panel shows up to four cards at a time. Scroll to see older entries.
Click the pushpin or press **P** on the selected card to keep it when history
fills or when you use **Clear unpinned**. The trash-can button removes a card,
including a pinned one. History holds up to 12 distinct entries of up to 64 KiB
each in memory until restart. Up to 11 can be pinned so there is room for new copies.

Keep one instance per field. Pass the field buffer, capacity (including its
terminating zero), and the owner's replace-selection flag to `key` or `insert`.
Set that flag when focus should select the existing value. Handle application
shortcuts before passing the key and its modifiers to `key`.
Use `input.draw(...)` to paint the text, selection, and caret. During a
field-only update, pass the window ID to present just that rectangle. Animate
the caret in the app loop and mark a field repaint as pending when calling
`cursor.animate` so it refreshes the saved column during the repaint.

The kernel shell terminal has a separate two-pixel underline, blinking every
500 ms while the shell owns input and its window is focused. Output/navigation
restarts its visible period. Its grid cursor supports CSI C/D movement across
wrapped lines; shell editing keeps command length separate from the insertion
position so moving left/right does not lose text. Up/down history still places
the insertion point at the end of the recalled command.
At the shell prompt, Ctrl+Shift+C copies the command line and Ctrl+V inserts
single-line text at the caret. The private console also accepts single-line
paste. The terminal keeps Ctrl+C for interrupting the foreground program.

Run `make test-text-input test-notepad test-explorer` for host regression tests.
